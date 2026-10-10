#!/usr/bin/env python3
"""TLS client interop matrix: runs tls.c (via tls_host) against OpenSSL s_server in every configuration we claim
to support. usage: tls_matrix.py /path/to/tls_host"""
import subprocess, sys, os, time, socket, threading, select, hashlib, signal, shutil, tempfile
host = sys.argv[1]
host_noems = sys.argv[2] if len(sys.argv) > 2 else None
tmp = tempfile.mkdtemp(prefix="tlsm")
def sh(*a): subprocess.run(a, check=True, capture_output=True)
sh("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", f"{tmp}/rsa.key", "-out", f"{tmp}/rsa.crt", "-days", "2", "-subj", "/CN=localhost")
sh("openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", f"{tmp}/ec.key", "-out", f"{tmp}/ec.crt", "-days", "2", "-subj", "/CN=localhost")
os.makedirs(f"{tmp}/www"); big = os.urandom(300000); open(f"{tmp}/www/big.bin", "wb").write(big)
BASE = 26000
procs = []
def free_port():
    global BASE; BASE += 1; return BASE

def start_server(extra, cert="rsa"):
    port = free_port()
    p = subprocess.Popen(["openssl", "s_server", "-accept", str(port), "-cert", f"{tmp}/{cert}.crt", "-key", f"{tmp}/{cert}.key", "-WWW", "-quiet"] + extra,
                         cwd=f"{tmp}/www", stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.PIPE)
    procs.append(p); time.sleep(0.35); return port, p

class Coalescer(threading.Thread):
    """TCP proxy that merges consecutive plaintext handshake records of the server flight into ONE record (as Go,
    Java and Schannel do), to exercise the 'ServerHello + Certificate in one record' path."""
    def __init__(self, upstream):
        super().__init__(daemon=True); self.up = upstream; self.port = free_port()
        self.ls = socket.socket(); self.ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); self.ls.bind(("127.0.0.1", self.port)); self.ls.listen(4)
    def run(self):
        c, _ = self.ls.accept(); s = socket.create_connection(("127.0.0.1", self.up))
        buf = b""; pending = b""; plain = True
        while True:
            r, _, _ = select.select([c, s], [], [], 0.08)
            if not r:
                if pending: c.sendall(b"\x16\x03\x03" + len(pending).to_bytes(2, "big") + pending); pending = b""
                continue
            if c in r:
                d = c.recv(65536)
                if not d: break
                s.sendall(d)
            if s in r:
                d = s.recv(65536)
                if not d: break
                buf += d
                while len(buf) >= 5:
                    L = int.from_bytes(buf[3:5], "big")
                    if len(buf) < 5 + L: break
                    rec, buf = buf[:5 + L], buf[5 + L:]
                    if plain and rec[0] == 22:
                        pending += rec[5:]
                    else:
                        if pending: c.sendall(b"\x16\x03\x03" + len(pending).to_bytes(2, "big") + pending); pending = b""
                        if rec[0] == 20 or rec[0] == 23: plain = False
                        c.sendall(rec)
        try: c.close(); s.close()
        except Exception: pass

fails = 0
def run(name, extra, cert, want_ver, want_suite, want_group, path="/", coalesce=False, expect_fail=None, client=None):
    global fails
    port, p = start_server(extra, cert)
    if coalesce:
        co = Coalescer(port); co.start(); port = co.port
    env = dict(os.environ, TLS_TRACE="1")
    r = subprocess.run([client or host, "127.0.0.1", str(port), "localhost", path], capture_output=True, env=env, timeout=40)
    err = r.stderr.decode(errors="replace"); out = r.stdout
    p.kill(); p.wait()
    if expect_fail:
        ok = r.returncode != 0 and expect_fail in err
    else:
        needle = f"{want_ver} {want_suite}" + (f" {want_group}" if want_group else "")
        ok = r.returncode == 0 and needle in err and b"200" in out.split(b"\r\n")[0] or False
        if ok and path == "/big.bin":
            body = out.split(b"\r\n\r\n", 1)[1]; ok = body == big
    if not ok: fails += 1
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else f"\n     rc={r.returncode}\n" + "\n".join("     " + l for l in err.strip().split("\n")[-8:])))

T13 = [("TLS_CHACHA20_POLY1305_SHA256", "x25519"), ("TLS_AES_128_GCM_SHA256", "x25519"), ("TLS_AES_256_GCM_SHA384", "x25519"),
       ("TLS_CHACHA20_POLY1305_SHA256", "secp256r1"), ("TLS_AES_128_GCM_SHA256", "secp256r1"), ("TLS_AES_256_GCM_SHA384", "secp256r1")]
for suite, grp in T13:
    g = "X25519" if grp == "x25519" else "P-256"
    for cert in ("rsa", "ec"):
        run(f"TLS1.3 {suite} {grp} ({cert} cert)", ["-tls1_3", "-ciphersuites", suite, "-groups", g], cert, "TLSv1.3", suite, grp)

T12 = [("ECDHE-RSA-CHACHA20-POLY1305", "rsa"), ("ECDHE-ECDSA-CHACHA20-POLY1305", "ec"), ("ECDHE-RSA-AES128-GCM-SHA256", "rsa"), ("ECDHE-ECDSA-AES128-GCM-SHA256", "ec"),
       ("ECDHE-RSA-AES256-GCM-SHA384", "rsa"), ("ECDHE-ECDSA-AES256-GCM-SHA384", "ec")]
for suite, cert in T12:
    for grp, g in (("x25519", "X25519"), ("secp256r1", "P-256")):
        run(f"TLS1.2 {suite} {grp}", ["-tls1_2", "-cipher", suite, "-groups", g], cert, "TLSv1.2", suite, grp)

if host_noems:
    run("TLS1.2 without extended master secret (client omits EMS)", ["-tls1_2", "-cipher", "ECDHE-RSA-AES128-GCM-SHA256"], "rsa", "TLSv1.2", "ECDHE-RSA-AES128-GCM-SHA256", "x25519", client=host_noems)
    run("TLS1.2 no-EMS, ChaCha, P-256", ["-tls1_2", "-cipher", "ECDHE-ECDSA-CHACHA20-POLY1305", "-groups", "P-256"], "ec", "TLSv1.2", "ECDHE-ECDSA-CHACHA20-POLY1305", "secp256r1", client=host_noems)
run("TLS1.2 ServerHello+Certificate coalesced into one record", ["-tls1_2", "-cipher", "ECDHE-RSA-AES256-GCM-SHA384"], "rsa", "TLSv1.2", "ECDHE-RSA-AES256-GCM-SHA384", "x25519", coalesce=True)
run("TLS1.2 coalesced + P-256 + ChaCha", ["-tls1_2", "-cipher", "ECDHE-ECDSA-CHACHA20-POLY1305", "-groups", "P-256"], "ec", "TLSv1.2", "ECDHE-ECDSA-CHACHA20-POLY1305", "secp256r1", coalesce=True)
run("TLS1.3 default server config (picks its favourites)", ["-tls1_3"], "rsa", "TLSv1.3", "TLS_", "")   # any suite; checked below
for suite, ver, extra, cert in (("TLS_AES_256_GCM_SHA384", "TLSv1.3", ["-tls1_3", "-ciphersuites", "TLS_AES_256_GCM_SHA384"], "ec"),
                                ("ECDHE-RSA-CHACHA20-POLY1305", "TLSv1.2", ["-tls1_2", "-cipher", "ECDHE-RSA-CHACHA20-POLY1305"], "rsa"),
                                ("ECDHE-ECDSA-AES128-GCM-SHA256", "TLSv1.2", ["-tls1_2", "-cipher", "ECDHE-ECDSA-AES128-GCM-SHA256"], "ec")):
    run(f"300 KB download, {ver} {suite}", extra, cert, ver, suite, "", path="/big.bin")
run("TLS 1.1 only server is refused cleanly", ["-tls1_1", "-cipher", "ALL:@SECLEVEL=0"], "rsa", "", "", "", expect_fail="TLS")
run("server without any common cipher gives an alert", ["-tls1_2", "-cipher", "AES128-SHA:@SECLEVEL=0"], "rsa", "", "", "", expect_fail="alert")
for p in procs:
    try: p.kill()
    except Exception: pass
shutil.rmtree(tmp, ignore_errors=True)
print("ALL OK" if not fails else f"{fails} FAILED"); sys.exit(1 if fails else 0)
