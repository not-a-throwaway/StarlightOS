#!/usr/bin/env python3
"""End-to-end test of libslweb's HTTPS path (M9.4).

Drives libslweb/http.c -- the browser's HTTP client -- over real loopback
sockets against OpenSSL s_server. The TLS handshake and X.509 validation are
the same tls.c/crypto.c/x509.c the kernel uses (via libslweb/tls_user.c), so
this covers everything below the ring-3 socket ABI.

    usage: slweb_tls_test.py /path/to/slweb_tls_host
"""
import os, socket, subprocess, sys, tempfile, time

host = sys.argv[1]
tmp = tempfile.mkdtemp(prefix="slwebtls")

def sh(*a):
    subprocess.run(a, check=True, capture_output=True)

# A small CA, a leaf for localhost and a leaf with a different name.
sh("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", f"{tmp}/ca.key",
   "-out", f"{tmp}/ca.pem", "-days", "2", "-subj", "/CN=slweb Test CA")
open(f"{tmp}/ca.ext", "w").write("basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n")

def leaf(name, cn, san):
    sh("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-keyout", f"{tmp}/{name}.key",
       "-out", f"{tmp}/{name}.csr", "-subj", f"/CN={cn}")
    open(f"{tmp}/{name}.ext", "w").write(
        f"subjectAltName={san}\nbasicConstraints=CA:FALSE\n"
        "keyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n")
    sh("openssl", "x509", "-req", "-in", f"{tmp}/{name}.csr", "-CA", f"{tmp}/ca.pem",
       "-CAkey", f"{tmp}/ca.key", "-CAcreateserial", "-out", f"{tmp}/{name}.crt",
       "-days", "2", "-extfile", f"{tmp}/{name}.ext")

leaf("localhost", "localhost", "DNS:localhost")
leaf("other", "other.test", "DNS:other.test")
# A self-signed cert to use as a wrong trust anchor.
sh("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", f"{tmp}/wrong.key",
   "-out", f"{tmp}/wrong.pem", "-days", "2", "-subj", "/CN=Wrong CA")

os.makedirs(f"{tmp}/www", exist_ok=True)
open(f"{tmp}/www/INDEX.HTM", "wb").write(b"<html><body><h1>Hello over TLS</h1></body></html>")

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

def start(cert):
    port = free_port()
    p = subprocess.Popen(["openssl", "s_server", "-accept", str(port), "-cert", f"{tmp}/{cert}.crt",
                          "-key", f"{tmp}/{cert}.key", "-WWW", "-quiet"],
                         cwd=f"{tmp}/www", stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         stdin=subprocess.PIPE)
    for _ in range(60):
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            return port, p
        except OSError:
            time.sleep(0.1)
    return port, p

fails = 0
def run(name, url, args=(), want=None, body_has=None):
    global fails
    r = subprocess.run([host, url, *args], capture_output=True, timeout=40)
    out = r.stdout.decode(errors="replace")
    d = {}
    for line in out.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            d[k] = v
    ok = d.get("error") is not None and all(str(d.get(k)) == str(v) for k, v in (want or {}).items())
    if ok and body_has:
        ok = body_has in d.get("body", "")
    if not ok:
        fails += 1
    print(("ok   " if ok else "FAIL ") + name)
    if not ok:
        print("     rc=%d\n     %s" % (r.returncode, out.replace("\n", "\n     ")))
    return d

port, p = start("localhost")
run("verified https fetch (cert, hostname and body)",
    f"https://localhost:{port}/INDEX.HTM", ["--cafile", f"{tmp}/ca.pem"],
    want=dict(error=0, status=200, tls_verified=1, tls_peer="localhost"),
    body_has="Hello over TLS")
run("no trust anchor fails closed",
    f"https://localhost:{port}/INDEX.HTM", [],
    want=dict(error=-7, tls_verified=0, tls_error=-17))
run("certificate signed by an unknown CA is rejected",
    f"https://localhost:{port}/INDEX.HTM", ["--cafile", f"{tmp}/wrong.pem"],
    want=dict(error=-7, tls_verified=0, tls_error=-17))
run("--insecure skips validation but still fetches",
    f"https://localhost:{port}/INDEX.HTM", ["--insecure"],
    want=dict(error=0, status=200, tls_verified=0), body_has="Hello over TLS")
p.kill(); p.wait()

port, p = start("other")
run("hostname mismatch is rejected even with a trusted CA",
    f"https://localhost:{port}/INDEX.HTM", ["--cafile", f"{tmp}/ca.pem"],
    want=dict(error=-7, tls_verified=0, tls_error=-17))
p.kill(); p.wait()

print("FAILURES" if fails else "ALL OK")
sys.exit(1 if fails else 0)
