#!/usr/bin/env python3
"""Check an OpenBird (or DoorBird) device the way the nymea DoorBird plugin uses it.

    tools/check_device.py --host bha-1ccae3aabbcc.local --user openbird --password changeme
    tools/check_device.py --discover            # needs: pip install zeroconf
    tools/check_device.py --host ... --ring     # also press the doorbell when asked
    tools/check_device.py --host ... --open     # also pulse relay 1 (opens the door!)
"""

import argparse
import base64
import http.server
import json
import socket
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

failures = 0
port = 80


def check(name, ok, detail=""):
    global failures
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f": {detail}" if detail else ""))
    if not ok:
        failures += 1
    return ok


def request(host, path, auth=None, method="GET", body=None, timeout=5):
    req = urllib.request.Request(f"http://{host}:{port}{path}", data=body, method=method)
    if auth:
        token = base64.b64encode(f"{auth[0]}:{auth[1]}".encode()).decode()
        req.add_header("Authorization", "Basic " + token)
    if body is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, dict(resp.headers), resp.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def discover(timeout=5):
    try:
        from zeroconf import ServiceBrowser, Zeroconf
    except ImportError:
        print("zeroconf not installed: pip install zeroconf")
        sys.exit(2)

    found = []

    class Listener:
        def add_service(self, zc, type_, name):
            info = zc.get_service_info(type_, name)
            if info:
                found.append(info)

        def update_service(self, *args):
            pass

        def remove_service(self, *args):
            pass

    zc = Zeroconf()
    ServiceBrowser(zc, "_axis-video._tcp.local.", Listener())
    time.sleep(timeout)
    zc.close()

    hosts = []
    for info in found:
        # The plugin requires host names starting with "bha-" and reads the MAC
        # from the first TXT record ("macaddress=...")
        server = info.server or ""
        txt = [f"{k.decode()}={(v or b'').decode()}" for k, v in info.properties.items()]
        addresses = info.parsed_addresses()
        print(f"  {info.name}  host={server}  addr={addresses}  txt={txt}")
        if not server.startswith("bha-"):
            continue
        check(f"{info.name}: host name starts with bha-", True, server)
        check(f"{info.name}: first TXT record is macaddress=", bool(txt) and txt[0].startswith("macaddress="),
              txt[0] if txt else "no TXT records")
        if addresses:
            hosts.append(addresses[0])
    check("discovered a DoorBird compatible device", bool(hosts))
    return hosts


def schedule_references(host, auth, fav_id):
    """True if any doorbell schedule output calls HTTP favorite fav_id."""
    status, _, body = request(host, "/bha-api/schedule.cgi?input=doorbell", auth)
    if status != 200:
        return False
    return any(o.get("event") == "http" and o.get("param") == fav_id
               for entry in json.loads(body) for o in entry.get("output", []))


def local_ip_towards(host):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((socket.gethostbyname(host), port))
        return s.getsockname()[0]


class CallbackServer:
    """Receives the HTTP notification the device sends for a schedule entry."""

    def __init__(self, bind_ip):
        self.hits = []
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                outer.hits.append(self.path)
                self.send_response(200)
                self.end_headers()

            def log_message(self, *args):
                pass

        self.server = http.server.HTTPServer((bind_ip, 0), Handler)
        self.url = f"http://{bind_ip}:{self.server.server_address[1]}/openbird-check"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()


def read_monitor(host, auth, seconds, stop_on=None):
    """Open monitor.cgi like the plugin and collect "name:state" messages."""
    token = base64.b64encode(f"{auth[0]}:{auth[1]}".encode()).decode()
    sock = socket.create_connection((host, port), timeout=seconds)
    sock.sendall((f"GET /bha-api/monitor.cgi?ring=doorbell,motionsensor HTTP/1.1\r\n"
                  f"Host: {host}\r\nAuthorization: Basic {token}\r\n\r\n").encode())
    buf = b""
    messages = []
    deadline = time.time() + seconds
    try:
        while time.time() < deadline:
            sock.settimeout(max(0.1, deadline - time.time()))
            try:
                chunk = sock.recv(1024)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
            # Same parsing as Doorbird::connectToEventMonitor()
            for part in buf.split(b"--ioboundary")[1:]:
                if b"Content-Type: text/plain" in part:
                    msg = part.split(b"Content-Type: text/plain", 1)[1].strip().decode()
                    if msg and msg not in messages[-1:]:
                        messages.append(msg)
            if stop_on and stop_on in messages:
                break
    finally:
        sock.close()
    header = buf.split(b"\r\n\r\n", 1)[0].decode(errors="replace")
    return header, messages


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", help="device host name or IP")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--user", default="openbird")
    parser.add_argument("--password", default="changeme")
    parser.add_argument("--discover", action="store_true", help="find the device via mDNS like the plugin")
    parser.add_argument("--ring", action="store_true", help="wait for a doorbell press")
    parser.add_argument("--ring-timeout", type=int, default=30, help="seconds to wait for the press")
    parser.add_argument("--open", action="store_true", help="pulse relay 1")
    args = parser.parse_args()
    global port
    port = args.port

    host = args.host
    if args.discover:
        hosts = discover()
        host = host or (hosts[0] if hosts else None)
    if not host:
        parser.error("no --host given and nothing discovered")
    auth = (args.user, args.password)
    print(f"Checking {host}")

    # The plugin sends no credentials except for getsession.cgi, Qt resends them
    # after a 401 with the same realm.
    try:
        status, headers, _ = request(host, "/bha-api/info.cgi")
    except OSError as e:
        print(f"Cannot reach {host}: {e}")
        return 2
    challenge = headers.get("WWW-Authenticate", "")
    check("unauthenticated request is answered with 401", status == 401, str(status))
    check("401 carries Basic realm=\"DoorBird\"", 'Basic realm="DoorBird"' in challenge, challenge)

    status, _, body = request(host, "/bha-api/getsession.cgi", auth)
    session = json.loads(body or b"{}").get("BHA", {}).get("SESSIONID", "") if status == 200 else ""
    if not check("getsession.cgi returns a SESSIONID", bool(session), f"HTTP {status}"):
        print("Credentials wrong? Stopping.")
        return 1
    status, _, _ = request(host, f"/bha-api/info.cgi?sessionid={session}")
    check("sessionid query parameter is accepted", status == 200, str(status))

    status, _, body = request(host, "/bha-api/info.cgi", auth)
    info = json.loads(body).get("BHA", {}).get("VERSION", [{}])[0] if status == 200 else {}
    check("info.cgi returns VERSION", bool(info), json.dumps(info))
    check("firmware >= 000110 (favorites/schedules)", int(info.get("FIRMWARE", "0")) >= 110, str(info.get("FIRMWARE")))

    header, messages = read_monitor(host, auth, 3)
    check("monitor.cgi is a multipart stream", "multipart/x-mixed-replace" in header and "--ioboundary" in header,
          header.splitlines()[0] if header else "no response")
    check("monitor.cgi reports the current doorbell state", any(m.startswith("doorbell:") for m in messages),
          str(messages))

    for path in ("/bha-api/favorites.cgi", "/bha-api/schedule.cgi"):
        status, _, body = request(host, path, auth)
        try:
            json.loads(body)
            valid = True
        except ValueError:
            valid = False
        check(f"{path} returns JSON", status == 200 and valid, str(status))

    # Favorite + schedule round trip, the way Home Assistant sets up notifications
    callback = CallbackServer(local_ip_towards(host))
    query = urllib.parse.urlencode({"action": "save", "type": "http", "title": "openbird-check", "value": callback.url})
    status, headers, _ = request(host, f"/bha-api/favorites.cgi?{query}", auth)
    fav_id = headers.get("favoriteid")
    check("favorite saved, id in favoriteid header", status == 200 and fav_id is not None, f"{status} id={fav_id}")
    if fav_id is not None:
        # Extend an existing doorbell entry instead of replacing it; removing the
        # favorite at the end strips only our output again.
        status, _, body = request(host, "/bha-api/schedule.cgi?input=doorbell", auth)
        existing = [e for e in json.loads(body) if e.get("param") == "1"] if status == 200 else []
        entry = existing[0] if existing else {"input": "doorbell", "param": "1", "output": []}
        entry["output"].append({"event": "http", "param": fav_id, "enabled": "1",
                                "schedule": {"weekdays": [{"from": "0", "to": "604799"}]}})
        status, _, _ = request(host, "/bha-api/schedule.cgi", auth, "POST", json.dumps(entry).encode())
        check("schedule entry saved", status == 200, str(status))
        check("schedule entry listed", schedule_references(host, auth, fav_id))

    if args.ring:
        print(f"Press the doorbell now ({args.ring_timeout} s) ...")
        _, messages = read_monitor(host, auth, args.ring_timeout, stop_on="doorbell:H")
        check("doorbell:H received on monitor.cgi", "doorbell:H" in messages, str(messages))
        if fav_id is not None:
            for _ in range(50):
                if callback.hits:
                    break
                time.sleep(0.1)
            check("HTTP favorite was called on ring", bool(callback.hits), str(callback.hits))

    if fav_id is not None:
        query = urllib.parse.urlencode({"action": "remove", "type": "http", "id": fav_id})
        status, _, _ = request(host, f"/bha-api/favorites.cgi?{query}", auth)
        check("favorite removed", status == 200, str(status))
        check("schedule output removed with its favorite", not schedule_references(host, auth, fav_id))

    if args.open:
        status, _, _ = request(host, "/bha-api/open-door.cgi?r=1", auth)
        check("open-door.cgi?r=1", status == 200, str(status))

    print(f"\n{failures} check(s) failed" if failures else "\nAll checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
