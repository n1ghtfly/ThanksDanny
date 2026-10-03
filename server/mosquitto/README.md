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
