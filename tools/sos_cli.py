#!/usr/bin/env python3
"""sos_cli - talk to the Sons of Sudo badges from a PC, through your Mosquitto.

Needs:  pip install paho-mqtt

  python sos_cli.py --host mqtt.example.com --user claudio --password ... watch
  python sos_cli.py --host ... --user claudio --password ... slap danny 8
  python sos_cli.py --host ... --user claudio --password ... who

Connection options can also come from environment variables, so the password is not typed on the
command line every time: SOS_HOST, SOS_PORT, SOS_USER, SOS_PASSWORD, SOS_TLS (1/0, default 1).

Protocol (what the badges speak - keep in step with Badge/Sos.ino):
  sos/<to>/inbox/<from>   "slap|<strength 1-10>"      the broker's ACL only lets <from> be YOUR login
  sos/presence/<name>     "1" online / "0" offline     retained; "0" is also each badge's last will
"""
import argparse, os, ssl, sys, time

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("needs paho-mqtt:  pip install paho-mqtt")

MEMBERS = ["claudio", "danny", "walter"]


def connect(a, on_message=None, subs=()):
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"sos-cli-{a.user}-{os.getpid()}")
    c.username_pw_set(a.user, a.password)
    if a.tls:
        c.tls_set(cert_reqs=ssl.CERT_REQUIRED)
    state = {"ok": None}

    def on_connect(cl, ud, flags, rc, props=None):
        state["ok"] = (rc == 0)
        if rc != 0:
            print(f"! broker refused the connection: {rc}")
        for s in subs:
            cl.subscribe(s, qos=1)

    c.on_connect = on_connect
    if on_message:
        c.on_message = on_message
    try:
        c.connect(a.host, a.port, keepalive=30)
    except ConnectionRefusedError:
        sys.exit(f"! {a.host}:{a.port} refused the connection - nothing is listening on that port.\n"
                 f"  Home test without a certificate? Add:  --port 1883 --no-tls\n"
                 f"  Otherwise check Mosquitto is running:  docker logs mosquitto --tail 30")
    except (TimeoutError, OSError) as e:
        sys.exit(f"! cannot reach {a.host}:{a.port} ({e}). Check the host name and that you are on the same network.")
    c.loop_start()
    for _ in range(100):
        if state["ok"] is not None:
            break
        time.sleep(0.05)
    if not state["ok"]:
        sys.exit("! could not connect - check host, port, TLS and login")
    return c


def cmd_slap(a):
    who = a.target.lower()
    if who not in MEMBERS:
        sys.exit(f"! unknown badge '{who}' - one of: {', '.join(MEMBERS)}")
    strength = max(1, min(10, a.strength))
    c = connect(a)
    info = c.publish(f"sos/{who}/inbox/{a.user}", f"slap|{strength}", qos=1)
    info.wait_for_publish(5)
    print(f"slapped {who} with strength {strength}" + ("" if info.is_published() else " (not confirmed by the broker)"))
    c.loop_stop(); c.disconnect()


def cmd_watch(a):
    def on_message(cl, ud, m):
        print(time.strftime("%H:%M:%S"), m.topic, m.payload.decode(errors="replace"), flush=True)
    connect(a, on_message, subs=("sos/#",) if a.user == "console" else
            (f"sos/{a.user}/inbox/+", "sos/presence/+", "sos/all/#"))
    print("watching - Ctrl+C to stop")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass


def cmd_who(a):
    seen = {}
    def on_message(cl, ud, m):
        seen[m.topic.rsplit("/", 1)[-1]] = m.payload.decode(errors="replace")
    c = connect(a, on_message, subs=("sos/presence/+",))
    time.sleep(1.5)                       # retained presence arrives right after subscribing
    for n in MEMBERS:
        print(f"  {n:8s} {'online' if seen.get(n) == '1' else 'offline'}")
    c.loop_stop(); c.disconnect()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=os.environ.get("SOS_HOST"))
    p.add_argument("--port", type=int, default=int(os.environ.get("SOS_PORT", "0")) or None)
    p.add_argument("--user", default=os.environ.get("SOS_USER"))
    p.add_argument("--password", default=os.environ.get("SOS_PASSWORD"))
    p.add_argument("--no-tls", dest="tls", action="store_false",
                   default=os.environ.get("SOS_TLS", "1") != "0", help="plain MQTT (LAN testing only)")
    sp = p.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("slap", help="slap a badge");  s.add_argument("target"); s.add_argument("strength", type=int, nargs="?", default=6)
    sp.add_parser("watch", help="print badge traffic as it happens")
    sp.add_parser("who", help="who is online")
    a = p.parse_args()
    if not (a.host and a.user and a.password):
        sys.exit("! need --host, --user and --password (or SOS_HOST / SOS_USER / SOS_PASSWORD)")
    a.port = a.port or (8883 if a.tls else 1883)
    {"slap": cmd_slap, "watch": cmd_watch, "who": cmd_who}[a.cmd](a)


if __name__ == "__main__":
    main()
