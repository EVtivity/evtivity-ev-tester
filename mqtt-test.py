#!/usr/bin/env python3
"""
Interactive MQTT console (terminal UI) for the EVtivity EV Tester.

Just run it - no arguments needed:

    python3 mqtt-test.py

A connection screen asks for the MQTT broker your device uses (host, port, topic, and
optional user/password); the values are remembered for next time in ~/.evtester-mqtt.json.
Then scroll the command list with the arrow keys (or j/k) and press Enter to send it to the
device. The device's live published state and availability show at the bottom, color-coded.

Commands map to the firmware's MQTT scheme:
  <topic>/mode  <- ready | charge | suspend | fault
  <topic>/song  <- hbd | xmas | canon | stop
and it subscribes to what the device publishes:
  <topic>/state         (ready | charging | suspended | faulted)
  <topic>/availability  (online | offline)

If you point at a local broker (localhost or this machine's own IP) and none is running, it
offers to install Mosquitto (via Homebrew on macOS) and start it for you.

Requires paho-mqtt:  pip install paho-mqtt
"""

import argparse
import contextlib
import curses
import json
import os
import platform
import shutil
import socket
import subprocess
import sys
import time
import warnings

warnings.filterwarnings("ignore")   # keep paho-mqtt 2.x DeprecationWarnings off the TUI

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt is not installed. Run:  pip install paho-mqtt")

CONFIG_PATH = os.path.expanduser("~/.evtester-mqtt.json")
BROKER_CONF = os.path.expanduser("~/.evtester-mosquitto.conf")
BROKER_LOG = os.path.expanduser("~/.evtester-mosquitto.log")

# (group, payload, label) - group is the sub-topic ("mode" or "song")
COMMANDS = [
    ("mode", "ready",   "Ready     - back to idle (charger shows Available)"),
    ("mode", "charge",  "Charge    - start a session (Preparing -> Charging)"),
    ("mode", "suspend", "Suspend   - connected, not charging (SuspendedEV)"),
    ("mode", "fault",   "Fault     - inject a pilot fault (only while charging)"),
    ("song", "hbd",     "Happy Birthday"),
    ("song", "xmas",    "Merry Christmas"),
    ("song", "canon",   "Canon in D"),
    ("song", "stop",    "stop the song"),
]
GROUP_TITLE = {"mode": "MODE  -> <topic>/mode", "song": "SONG  -> <topic>/song"}

C_TITLE, C_OK, C_ALERT, C_WARN, C_DIM, C_ACCENT = 1, 2, 3, 4, 5, 6
STATE_COLOR = {"charging": C_OK, "faulted": C_ALERT, "suspended": C_WARN, "ready": C_ACCENT}


def load_config():
    try:
        with open(CONFIG_PATH) as f:
            return json.load(f)
    except Exception:
        return {}


def save_config(vals):
    try:
        with open(CONFIG_PATH, "w") as f:
            json.dump({k: vals.get(k, "") for k in ("host", "port", "topic", "user")}, f)
    except Exception:
        pass


# --- Local-broker helpers: offer to install/start Mosquitto when aimed at a local broker ---
def _tcp_open(host, port, timeout=0.7):
    try:
        with socket.create_connection((host, int(port)), timeout=timeout):
            return True
    except OSError:
        return False


def _lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))       # no packets sent; just selects the outbound interface
        return s.getsockname()[0]
    except OSError:
        return ""
    finally:
        s.close()


def _is_local_host(host):
    return host in ("", "127.0.0.1", "localhost", "::1") or host == _lan_ip()


def _mosquitto_bin():
    found = shutil.which("mosquitto")
    if found:
        return found
    for p in ("/opt/homebrew/sbin/mosquitto", "/usr/local/sbin/mosquitto",
              "/usr/sbin/mosquitto", "/usr/bin/mosquitto"):
        if os.path.exists(p):
            return p
    return None


def _write_broker_conf(port):
    try:
        with open(BROKER_CONF, "w") as f:
            f.write("# EVtivity EV Tester local test broker - bench/LAN use only, do not expose to the internet.\n")
            f.write("listener %d 0.0.0.0\n" % int(port))
            f.write("allow_anonymous true\n")
        return BROKER_CONF
    except OSError:
        return None


def ensure_local_broker(host, port, skip=False):
    """Before the UI opens: if aimed at a local broker that isn't up, offer to install/start Mosquitto."""
    if skip or not _is_local_host(host):
        return
    if _tcp_open("127.0.0.1", port):
        return                                   # a broker is already reachable
    print("No MQTT broker is reachable at %s:%d." % (host or "127.0.0.1", port))
    mosq = _mosquitto_bin()
    if not mosq:
        if platform.system() == "Darwin" and shutil.which("brew"):
            if input("Install the Mosquitto broker now with Homebrew? [y/N] ").strip().lower().startswith("y"):
                subprocess.run(["brew", "install", "mosquitto"])
                mosq = _mosquitto_bin()
        elif platform.system() == "Linux":
            print("Install it with your package manager, e.g.  sudo apt install mosquitto")
        else:
            print("Install Mosquitto from https://mosquitto.org/download/")
        if not mosq:
            print("Continuing without a local broker - set a reachable broker on the next screen.\n")
            return
    if input("Start a local Mosquitto broker now? [Y/n] ").strip().lower().startswith("n"):
        return
    conf = _write_broker_conf(port)
    if not conf:
        print("Could not write the broker config; start Mosquitto manually.\n")
        return
    try:
        proc = subprocess.Popen([mosq, "-c", conf],
                                stdout=open(BROKER_LOG, "a"), stderr=subprocess.STDOUT,
                                start_new_session=True)
    except OSError as e:
        print("Failed to start Mosquitto: %s\n" % e)
        return
    for _ in range(20):
        if _tcp_open("127.0.0.1", port):
            lan = _lan_ip()
            print("Local broker running (pid %d) on port %d." % (proc.pid, port))
            if lan:
                print("Point the tester's MQTT broker at %s:%d (topic evtester)." % (lan, port))
            print("Stop it later with:  kill %d\n" % proc.pid)
            return
        time.sleep(0.15)
    print("Started Mosquitto but it isn't answering yet; check %s\n" % BROKER_LOG)


class Console:
    def __init__(self, host, port, topic, user, password):
        self.host, self.port, self.topic = host, port, topic
        self.sel = 0
        self.status = "connecting..."
        self.connected = False
        self.t0 = time.time()
        self.dev_state = "-"
        self.dev_avail = "-"
        self.log = []

        # paho-mqtt 2.x changed the callback signatures; pin VERSION1 so this works on 1.x and 2.x.
        if hasattr(mqtt, "CallbackAPIVersion"):
            self.cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)
        else:
            self.cli = mqtt.Client()
        if user:
            self.cli.username_pw_set(user, password or "")
        self.cli.on_connect = self._on_connect
        self.cli.on_disconnect = self._on_disconnect
        self.cli.on_message = self._on_message
        self.cli.connect_async(host, port, keepalive=30)
        self.cli.loop_start()

    def _on_connect(self, cli, u, flags, rc):
        if rc == 0:
            self.connected, self.status = True, "CONNECTED"
            cli.subscribe(f"{self.topic}/state")
            cli.subscribe(f"{self.topic}/availability")
        else:
            self.connected, self.status = False, f"connect failed (rc={rc})"

    def _on_disconnect(self, cli, u, rc):
        self.connected, self.status = False, "disconnected (retrying)"

    def _on_message(self, cli, u, msg):
        payload = msg.payload.decode("utf-8", "replace")
        if msg.topic.endswith("/state"):
            self.dev_state = payload
        elif msg.topic.endswith("/availability"):
            self.dev_avail = payload

    def send(self):
        group, payload, _ = COMMANDS[self.sel]
        full = f"{self.topic}/{group}"
        self.cli.publish(full, payload, qos=0, retain=False)
        self.log.append((time.strftime("%H:%M:%S"), f"{full}  {payload}"))
        self.log = self.log[-8:]

    def close(self):
        self.cli.loop_stop()
        self.cli.disconnect()


def _put(win, y, x, text, w, attr=0):
    if 0 <= y and 0 <= x < w - 1:
        try:
            win.addnstr(y, x, text, w - 1 - x, attr)
        except curses.error:
            pass


def init_colors():
    curses.use_default_colors()
    for pid, fg in ((C_TITLE, curses.COLOR_CYAN), (C_OK, curses.COLOR_GREEN),
                    (C_ALERT, curses.COLOR_RED), (C_WARN, curses.COLOR_YELLOW),
                    (C_ACCENT, curses.COLOR_CYAN), (C_DIM, curses.COLOR_WHITE)):
        curses.init_pair(pid, fg, -1)


def conn_form(scr, vals):
    """Editable connection form. Returns the settings dict, or None if the user quits."""
    fields = [("host", "Broker host / IP"), ("port", "Port"), ("topic", "Base topic"),
              ("user", "Username (blank if none)"), ("password", "Password (blank if none)")]
    idx = 0
    curses.curs_set(1)
    scr.nodelay(False)
    scr.keypad(True)
    cp = curses.color_pair
    while True:
        scr.erase()
        h, w = scr.getmaxyx()
        try:
            scr.box()
        except curses.error:
            pass
        _put(scr, 0, 2, " EVtivity EV Tester - connect ", w, cp(C_TITLE) | curses.A_BOLD)
        _put(scr, 2, 2, "MQTT broker your device is configured to use:", w, cp(C_DIM))
        box_x, field_w = 30, 34
        cy = cx = None
        for i, (key, label) in enumerate(fields):
            y = 4 + i * 2
            _put(scr, y, 3, f"{label}", w, cp(C_ACCENT) | curses.A_BOLD if i == idx else 0)
            val = vals.get(key, "")
            shown = "*" * len(val) if key == "password" else val
            attr = curses.A_REVERSE if i == idx else curses.A_UNDERLINE
            _put(scr, y, box_x, (shown + " " * field_w)[:field_w], w, attr)
            if i == idx:
                cy, cx = y, box_x + min(len(shown), field_w - 1)
        _put(scr, h - 2, 3, "Tab / arrows: move field    type to edit    Enter: connect    Esc: quit", w, cp(C_DIM))
        if cy is not None:
            try:
                scr.move(cy, cx)
            except curses.error:
                pass
        scr.refresh()
        ch = scr.getch()
        if ch == 27:                                   # Esc
            return None
        elif ch in (curses.KEY_ENTER, 10, 13):
            if vals.get("host", "").strip():
                return vals
        elif ch in (9, curses.KEY_DOWN):
            idx = (idx + 1) % len(fields)
        elif ch in (curses.KEY_UP, curses.KEY_BTAB):
            idx = (idx - 1) % len(fields)
        elif ch in (curses.KEY_BACKSPACE, 127, 8):
            k = fields[idx][0]
            vals[k] = vals.get(k, "")[:-1]
        elif 32 <= ch <= 126:
            k = fields[idx][0]
            if k == "port" and not chr(ch).isdigit():
                continue
            vals[k] = vals.get(k, "") + chr(ch)


def draw(scr, c):
    scr.erase()
    h, w = scr.getmaxyx()
    try:
        scr.box()
    except curses.error:
        pass
    cp = curses.color_pair

    def hrule(y):
        try:
            scr.addch(y, 0, curses.ACS_LTEE)
            scr.hline(y, 1, curses.ACS_HLINE, w - 2)
            scr.addch(y, w - 1, curses.ACS_RTEE)
        except curses.error:
            pass

    _put(scr, 0, 2, " EVtivity EV Tester - MQTT console ", w, cp(C_TITLE) | curses.A_BOLD)
    _put(scr, 1, 2, f"broker {c.host}:{c.port}   topic {c.topic}", w)
    if c.connected:
        stat, col = "CONNECTED", C_OK
    elif "failed" in c.status:
        stat, col = c.status, C_ALERT
    elif time.time() - c.t0 > 5:
        stat, col = "no broker reachable at this host:port", C_ALERT
    else:
        stat, col = "connecting...", C_WARN
    badge = f"[ {stat} ]"
    _put(scr, 1, max(2, w - len(badge) - 3), badge, w, cp(col) | curses.A_BOLD)

    hrule(2)
    _put(scr, 3, 2, "Commands", w, cp(C_ACCENT) | curses.A_BOLD)

    row = 4
    last_group = None
    for i, (group, payload, label) in enumerate(COMMANDS):
        if group != last_group:
            _put(scr, row, 2, GROUP_TITLE[group], w, cp(C_DIM) | curses.A_BOLD)
            row += 1
            last_group = group
        if i == c.sel:
            _put(scr, row, 3, f"> {label}".ljust(w - 6), w, curses.A_REVERSE | curses.A_BOLD)
        else:
            _put(scr, row, 3, f"  {label}", w)
        row += 1

    hrule(row); row += 1
    _put(scr, row, 2, "Device", w, cp(C_ACCENT) | curses.A_BOLD)
    _put(scr, row, 11, "state", w)
    _put(scr, row, 17, f"{c.dev_state:<10}", w, cp(STATE_COLOR.get(c.dev_state, C_DIM)) | curses.A_BOLD)
    _put(scr, row, 30, "availability", w)
    _put(scr, row, 43, c.dev_avail, w, cp(C_OK if c.dev_avail == "online" else C_ALERT) | curses.A_BOLD)
    row += 1

    hrule(row); row += 1
    _put(scr, row, 2, "Sent", w, cp(C_ACCENT) | curses.A_BOLD)
    row += 1
    for t, text in reversed(c.log):
        if row >= h - 2:
            break
        _put(scr, row, 3, f"{t}  -> {text}", w, cp(C_DIM))
        row += 1

    hint = " up/down or j/k: scroll   Enter: send   q: quit "
    _put(scr, h - 1, max(2, (w - len(hint)) // 2), hint, w, cp(C_DIM))
    scr.refresh()


def console_loop(scr, c):
    curses.curs_set(0)
    scr.nodelay(True)
    scr.keypad(True)
    while True:
        draw(scr, c)
        ch = scr.getch()
        if ch == -1:
            time.sleep(0.05)
            continue
        if ch in (ord("q"), 27):
            break
        elif ch in (curses.KEY_UP, ord("k")):
            c.sel = (c.sel - 1) % len(COMMANDS)
        elif ch in (curses.KEY_DOWN, ord("j")):
            c.sel = (c.sel + 1) % len(COMMANDS)
        elif ch in (curses.KEY_ENTER, 10, 13):
            c.send()


def app(scr, defaults):
    init_colors()
    vals = conn_form(scr, defaults)
    if vals is None:
        return
    save_config(vals)
    port = int(vals.get("port") or 1883)
    c = Console(vals["host"].strip(), port, (vals.get("topic") or "evtester").strip(),
                vals.get("user") or None, vals.get("password") or None)
    try:
        console_loop(scr, c)
    finally:
        c.close()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="Interactive MQTT console (TUI) for the EVtivity EV Tester")
    ap.add_argument("--host", help="optional: pre-fill the broker host (otherwise entered in the UI)")
    ap.add_argument("--port", type=int)
    ap.add_argument("--topic")
    ap.add_argument("--user")
    ap.add_argument("--no-broker-setup", action="store_true",
                    help="skip the local-broker detect/install/start prompt")
    args = ap.parse_args()

    cfg = load_config()
    defaults = {
        "host":     args.host or cfg.get("host", ""),
        "port":     str(args.port) if args.port else cfg.get("port", "1883"),
        "topic":    args.topic or cfg.get("topic", "evtester"),
        "user":     args.user or cfg.get("user", ""),
        "password": "",
    }

    try:
        port = int(defaults["port"])
    except (TypeError, ValueError):
        port = 1883
    try:
        ensure_local_broker(defaults["host"], port, skip=args.no_broker_setup)
    except (KeyboardInterrupt, EOFError):
        print()
    curses.wrapper(app, defaults)
