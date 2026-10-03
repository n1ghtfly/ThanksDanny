# Mosquitto for the Sons of Sudo badges

One broker, three badges (Claudio, Danny, Walter), one login each. The badges speak:

| Topic | Payload | Who may write / read |
|---|---|---|
| `sos/<to>/inbox/<from>` | `slap|<1-10>` | anyone writes, but only with **their own** name as `<from>`; only `<to>` reads |
| `sos/presence/<name>` | `1` / `0` (retained, `0` is the last will) | each badge writes its own, everyone reads |
| `sos/all/#` | (group games, later) | everyone |

The ACL enforces the sender: a badge cannot slap in someone else's name, read someone else's inbox, or
fake someone else's online status. All of that was tested against Mosquitto 2.0.22 with exactly the
`mosquitto.conf` and `acl` in this folder, along with offline delivery (a slap sent to a badge that is
off lands when it reconnects) and the last will (a badge that loses power shows offline).

## 1. Logins

On the machine running Mosquitto:

    mosquitto_passwd -c passwd claudio      # -c only for the first one: it creates the file
    mosquitto_passwd passwd danny
    mosquitto_passwd passwd walter
    mosquitto_passwd passwd console         # for tools/sos_cli.py on a PC (optional)

Give each person only their own password. It is typed once into that person's badge, on the phone
setup page; it is never stored in the code or on GitHub.

## 2. Config and access rules

- New broker: use `mosquitto.conf` and `acl` from this folder as they are (Docker: `docker-compose.yml`,
  put both files plus `passwd` in `config/`).
- **Existing broker** (e.g. you already run one for Home Assistant): add the lines from `mosquitto.conf`
  you are missing, and **careful with the ACL**: once `acl_file` is set, every user is denied anything
  the file does not allow. Add your existing users at the bottom of `acl`, for example:

      user homeassistant
      topic readwrite #

## 3. Reaching it from outside your home

If your broker's name resolves to a private address (192.168.x.x, 10.x.x.x), that works for badges on
your home Wi-Fi only. For badges at other people's homes:

1. Let DuckDNS point to your **public** IP: run the DuckDNS updater on a device inside your network
   (router, NAS, the Pi itself) without an `ip=` value, so DuckDNS records the address the request came from.
2. Router: forward **TCP 8883** to the broker machine's LAN address, port 8883.
3. Do **not** forward 1883 (unencrypted).

## 4. Certificate (Let's Encrypt via DuckDNS)

The badges check the certificate against the public CA list, so it must come from a public CA - a
self-signed certificate is refused. Let's Encrypt can issue one for a DuckDNS name using the **DNS
challenge**, which works even if no web server is reachable. With acme.sh:

    export DuckDNS_Token="your-duckdns-token"
    acme.sh --issue --dns dns_duckdns -d <your-broker>.duckdns.org
    acme.sh --install-cert -d <your-broker>.duckdns.org \
        --key-file       /path/to/certs/privkey.pem \
        --fullchain-file /path/to/certs/fullchain.pem \
        --reloadcmd      "docker restart sos-mosquitto"

acme.sh renews it automatically and runs the reload command each time. Make sure the Mosquitto user can
read `privkey.pem`.

## 5. Quick test at home before the certificate exists

Uncomment `listener 1883` in `mosquitto.conf`, restart Mosquitto, and on the badge's phone setup page
use port **1883** with **TLS unticked**. Only on your own network - switch back to 8883 + TLS once the
certificate is in place.

## 6. Check it from a PC

    pip install paho-mqtt
    python tools/sos_cli.py --host <your-broker>.duckdns.org --user console --password ... watch
    python tools/sos_cli.py --host <your-broker>.duckdns.org --user claudio --password ... slap danny 8
    python tools/sos_cli.py --host <your-broker>.duckdns.org --user claudio --password ... who

(add `--no-tls --port 1883` for the home test of step 5). With one badge, you can slap it from the PC
and watch it react.

## 7. If you run IOTstack (Mosquitto in Docker)

IOTstack keeps Mosquitto's files in `~/IOTstack/volumes/mosquitto/` (container name `mosquitto`).

    docker exec mosquitto mosquitto_passwd -b /mosquitto/pwfile/pwfile claudio PASS1
    docker exec mosquitto mosquitto_passwd -b /mosquitto/pwfile/pwfile danny   PASS2
    docker exec mosquitto mosquitto_passwd -b /mosquitto/pwfile/pwfile walter  PASS3
    docker exec mosquitto mosquitto_passwd -b /mosquitto/pwfile/pwfile nodered PASS4   # your other clients

Put this folder's `acl` in `~/IOTstack/volumes/mosquitto/config/sos.acl` and add a
`user <name>` + `topic readwrite #` pair for each existing client (Node-RED, Home Assistant, ...).
In `~/IOTstack/volumes/mosquitto/config/mosquitto.conf`:

    listener 1883
    allow_anonymous false
    password_file /mosquitto/pwfile/pwfile
    acl_file /mosquitto/config/sos.acl

Then `cd ~/IOTstack && docker-compose restart mosquitto` and check `docker logs mosquitto --tail 20`.
Give Node-RED's MQTT broker node its new login. Mosquitto itself stays on plain 1883 inside the LAN.

## 8. If you run Nginx Proxy Manager in front (TLS for outside access)

NPM's Proxy Hosts are HTTP-only; MQTT needs a TCP **stream**. NPM includes
`/data/nginx/custom/stream.conf` in its stream block, so TLS can terminate there with the Let's Encrypt
certificate NPM already manages - Mosquitto then needs no certificate at all.

1. NPM -> SSL Certificates -> Let's Encrypt for `<your-broker>.duckdns.org` (DNS challenge with
   DuckDNS if port 80 does not reach NPM).
2. In NPM's shell: `ls /etc/letsencrypt/live/` - note the `npm-N` folder of that certificate.
3. Create `/data/nginx/custom/stream.conf`:

       server {
           listen 8883 ssl;
           ssl_certificate     /etc/letsencrypt/live/npm-N/fullchain.pem;
           ssl_certificate_key /etc/letsencrypt/live/npm-N/privkey.pem;
           proxy_pass <mosquitto-LAN-IP>:1883;
       }

   then `nginx -t && nginx -s reload` (or `openresty -t && systemctl reload openresty`). If NPM runs in
   Docker, also publish `8883:8883`.
4. Router: forward TCP 8883 to the NPM machine. Nothing for 1883.
5. Badges and `sos_cli.py`: host `<your-broker>.duckdns.org`, port 8883, TLS on.

Note: when the broker name resolves to your public IP, a home-network test must use Mosquitto's LAN IP
directly (port 1883, no TLS) until step 4 is done.
