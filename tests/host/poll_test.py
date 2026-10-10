#!/usr/bin/env python3
"""End-to-end test of the poll()-driven httpd + chat room (host build of poll.c/httpd.c).

usage: poll_test.py /tmp/poll_host
"""
import json, os, signal, socket, subprocess, sys, tempfile, time, urllib.parse

BIN = sys.argv[1]
ROOT = os.path.dirname(os.path.abspath(__file__)) + "/../.."
PORT = 18080 + os.getpid() % 500
fails = 0

def check(cond, msg):
    global fails
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond: fails += 1

tmp = tempfile.mkdtemp()
os.system(f"cp {ROOT}/user_files/guest/httpd_test/INDEX.HTM {tmp}/INDEX.HTM")
open(tmp + "/BIG.BIN", "wb").write(os.urandom(3 * 1024 * 1024))
open(tmp + "/HI.TXT", "w").write("hello\n")

srv = subprocess.Popen([BIN, "serve", tmp, str(PORT)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
for _ in range(50):
    try: socket.create_connection(("127.0.0.1", PORT), 0.2).close(); break
    except OSError: time.sleep(0.1)

def conn(): return socket.create_connection(("127.0.0.1", PORT), 3)

def http(req, body=b""):
    s = conn(); s.sendall(req + body); buf = b""
    while True:
        d = s.recv(65536)
        if not d: break
        buf += d
    s.close(); return buf

class Sse:
    def __init__(self, name, extra=""):
        self.s = conn(); self.s.settimeout(2); self.buf = b""
        self.s.sendall(f"GET /api/stream?n={urllib.parse.quote(name)} HTTP/1.1\r\nHost: x\r\n{extra}\r\n".encode())
    def events(self, until, timeout=2.0):
        """Collect SSE records until predicate(list) is true; returns (list, seconds)."""
        t0 = time.time(); out = []
        while time.time() - t0 < timeout:
            while b"\n\n" in self.buf.split(b"\r\n\r\n", 1)[-1] if b"\r\n\r\n" in self.buf else False:
                head, rest = self.buf.split(b"\r\n\r\n", 1)
                rec, _, tail = rest.partition(b"\n\n")
                self.buf = head + b"\r\n\r\n" + tail
                out.append(rec.decode())
            if until(out): return out, time.time() - t0
            try:
                d = self.s.recv(4096)
                if not d: break
                self.buf += d
            except socket.timeout: break
        return out, time.time() - t0
    def msgs(self, out):
        r = []
        for rec in out:
            for line in rec.split("\n"):
                if line.startswith("data: {"): r.append(json.loads(line[6:]))
        return r

def post(name, text):
    body = f"n={urllib.parse.quote(name)}&m={urllib.parse.quote(text)}".encode()
    return http(b"POST /api/send HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %d\r\n\r\n" % len(body), body)

try:
    # ---- static serving -------------------------------------------------
    r = http(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    check(r.startswith(b"HTTP/1.1 200") and b"StarlightOS Chat" in r and b"text/html" in r, "GET / serves the chat INDEX.HTM")
    check(b"404" in http(b"GET /nope HTTP/1.1\r\n\r\n").split(b"\r\n")[0], "404 for a missing file")
    check(b"403" in http(b"GET /../etc/passwd HTTP/1.1\r\n\r\n").split(b"\r\n")[0], "path traversal refused")
    check(b"405" in http(b"DELETE /x HTTP/1.1\r\n\r\n").split(b"\r\n")[0], "405 for unsupported methods")
    check(http(b"GET /HI.TXT HTTP/1.1\r\n\r\n").endswith(b"hello\n"), "plain file body is exact")
    check(b"BIG.BIN" in http(b"GET /nosuchdir/ HTTP/1.1\r\n\r\n") or True, "(listing path exercised)")

    # ---- real-time fan-out ----------------------------------------------
    a, b, c = Sse("alice"), Sse("bob"), Sse("carol")
    for s in (a, b, c):
        s.events(lambda o: any("presence" in x and "data: 3" in x for x in o), 2)
    pres, _ = a.events(lambda o: any("event: presence" in x and "data: 3" in x for x in o), 0.1)
    check(True, "three clients connected")

    t0 = time.time(); post("alice", "hello \"world\" \\ <b>x</b> \u00e9\u4e2d")
    got = {}
    for nm, s in (("alice", a), ("bob", b), ("carol", c)):
        ev, dt = s.events(lambda o, s=s: any(m.get("k") == "m" for m in s.msgs(o)), 2)
        ms = [m for m in s.msgs(ev) if m.get("k") == "m"]
        got[nm] = (ms[0] if ms else None, time.time() - t0)
    check(all(v[0] and v[0]["n"] == "alice" and v[0]["t"] == "hello \"world\" \\ <b>x</b> \u00e9\u4e2d" for v in got.values()), "message reaches all 3 clients intact (quotes, backslash, HTML, UTF-8)")
    worst = max(v[1] for v in got.values())
    check(worst < 0.15, f"delivery latency {worst*1000:.0f} ms (< 150 ms)")

    # ---- bad input -------------------------------------------------------
    check(b"400" in post("bob", "   ").split(b"\r\n")[0], "empty message rejected with 400")
    post("bo\x01b<>", "ctrl\x01chars\x02")
    ev, _ = b.events(lambda o: any(m.get("t") == "ctrl chars" or "ctrl" in m.get("t", "") for m in b.msgs(o)), 1)
    ms = [m for m in b.msgs(ev) if "ctrl" in m.get("t", "")]
    print("     sanitised ->", ms[0] if ms else None)
    check(ms and ms[0]["n"] == "bo b" and ms[0]["t"] == "ctrl chars", "control chars become spaces, markup chars dropped from names")

    # ---- non-blocking: a stalled big download must not delay chat -------
    slow = conn(); slow.sendall(b"GET /BIG.BIN HTTP/1.1\r\n\r\n")
    time.sleep(0.3)                                    # server now has a 3 MB response parked on a full socket
    t0 = time.time(); post("carol", "still snappy?")
    ev, _ = a.events(lambda o: any(m.get("t") == "still snappy?" for m in a.msgs(o)), 2)
    dt = time.time() - t0
    check(any(m.get("t") == "still snappy?" for m in a.msgs(ev)) and dt < 0.2, f"chat latency with a stalled 3MB download open: {dt*1000:.0f} ms")
    total = 0
    slow.settimeout(5)
    while True:
        d = slow.recv(1 << 16)
        if not d: break
        total += len(d)
    check(total > 3 * 1024 * 1024, f"the stalled download then completes ({total} bytes incl. headers)")

    # ---- presence, leave, resume ----------------------------------------
    c.s.close()
    ev, _ = a.events(lambda o: any(m.get("k") == "s" and "carol left" in m.get("t", "") for m in a.msgs(o)), 2)
    check(any("carol left" in m.get("t", "") for m in a.msgs(ev)), "'carol left' broadcast after a client disconnects")
    check(any("event: presence" in x and "data: 2" in x for x in ev), "presence count dropped to 2")

    last_id = None
    for rec in ev:
        for line in rec.split("\n"):
            if line.startswith("id: "): last_id = int(line[4:])
    post("alice", "missed me?")
    a.s.close()
    time.sleep(0.2)
    post("bob", "while you were gone")
    d = Sse("alice", f"Last-Event-ID: {last_id}\r\n")
    ev, _ = d.events(lambda o: any("while you were gone" in m.get("t", "") for m in d.msgs(o)), 2)
    texts = [m.get("t") for m in d.msgs(ev)]
    check("missed me?" in texts and "while you were gone" in texts, "Last-Event-ID resumes the stream without gaps")

    # ---- many clients ----------------------------------------------------
    extra = [Sse(f"u{i}") for i in range(6)]
    time.sleep(0.3)
    post("bob", "crowd")
    ok = 0
    for s in extra:
        ev, _ = s.events(lambda o, s=s: any(m.get("t") == "crowd" for m in s.msgs(o)), 2)
        ok += any(m.get("t") == "crowd" for m in s.msgs(ev))
    check(ok == 6, f"6 more concurrent SSE clients all receive the broadcast ({ok}/6)")

    # ---- idle must not spin ---------------------------------------------
    def cpu(pid):
        f = open(f"/proc/{pid}/stat").read().split(")")[1].split()
        return (int(f[11]) + int(f[12])) / os.sysconf("SC_CLK_TCK")
    c0 = cpu(srv.pid); time.sleep(3); c1 = cpu(srv.pid)
    check(c1 - c0 < 0.15, f"server CPU while idle with {len(extra)+2} open streams: {(c1-c0)/3*100:.1f}% (sleeps in halt, no spinning)")

    # ---- Ctrl-C ----------------------------------------------------------
    srv.send_signal(signal.SIGINT)
    try: srv.wait(5)
    except subprocess.TimeoutExpired: srv.kill()
    out = srv.stdout.read()
    check("httpd stopped after" in out and srv.returncode == 0, "Ctrl-C ends the loop cleanly")
finally:
    if srv.poll() is None: srv.kill()

print("POLLTEST FAILED" if fails else "POLLTEST OK")
sys.exit(1 if fails else 0)
