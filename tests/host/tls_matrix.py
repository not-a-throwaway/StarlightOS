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
sh("openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:secp384r1", "-nodes", "-keyout", f"{tmp}/ec384.key", "-out", f"{tmp}/ec384.crt", "-days", "2", "-subj", "/CN=localhost")
# A self-signed cert for a *different* key with the same name: usable as a wrong trust anchor.
sh("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", f"{tmp}/wrong.key", "-out", f"{tmp}/wrong.crt", "-days", "2", "-subj", "/CN=localhost")
CA = f"{tmp}/ca.pem"
with open(CA, "wb") as f:
    for c in ("rsa", "ec", "ec384"): f.write(open(f"{tmp}/{c}.crt", "rb").read())
# A three-level hierarchy (root CA -> intermediate CA -> leaf) so the TLS path is exercised with a
# real multi-certificate Certificate message, not just a self-signed leaf.
def wf(name, txt): open(f"{tmp}/{name}", "w").write(txt)
wf("ca.ext", "basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n")
sh("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", f"{tmp}/root.key", "-out", f"{tmp}/root.crt", "-days", "2", "-subj", "/CN=Test Root CA")
sh("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-keyout", f"{tmp}/inter.key", "-out", f"{tmp}/inter.csr", "-subj", "/CN=Test Intermediate CA")
sh("openssl", "x509", "-req", "-in", f"{tmp}/inter.csr", "-CA", f"{tmp}/root.crt", "-CAkey", f"{tmp}/root.key", "-CAcreateserial", "-out", f"{tmp}/inter.crt", "-days", "2", "-extfile", f"{tmp}/ca.ext")
wf("leaf.ext", "subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n")
sh("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-keyout", f"{tmp}/leaf.key", "-out", f"{tmp}/leaf.csr", "-subj", "/CN=localhost")
sh("openssl", "x509", "-req", "-in", f"{tmp}/leaf.csr", "-CA", f"{tmp}/inter.crt", "-CAkey", f"{tmp}/inter.key", "-CAcreateserial", "-out", f"{tmp}/leaf.crt", "-days", "2", "-extfile", f"{tmp}/leaf.ext")
os.makedirs(f"{tmp}/www"); big = os.urandom(300000); open(f"{tmp}/www/big.bin", "wb").write(big)
BASE = 26000
procs = []
def free_port():
    global BASE; BASE += 1; return BASE

def start_server(extra, cert="rsa", chain=None):
    port = free_port()
    cmd = ["openssl", "s_server", "-accept", str(port), "-cert", f"{tmp}/{cert}.crt", "-key", f"{tmp}/{cert}.key", "-WWW", "-quiet"]
    if chain: cmd += ["-cert_chain", f"{tmp}/{chain}.crt"]
    p = subprocess.Popen(cmd + extra,
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
def run(name, extra, cert, want_ver, want_suite, want_group, path="/", coalesce=False, expect_fail=None,
        client=None, name_override="localhost", ca=CA, insecure=False, chain=None):
    global fails
    port, p = start_server(extra, cert, chain)
    if coalesce:
        co = Coalescer(port); co.start(); port = co.port
    env = dict(os.environ, TLS_TRACE="1")
    if insecure: env["TLS_INSECURE"] = "1"
    elif ca: env["TLS_CAFILE"] = ca
    r = subprocess.run([client or host, "127.0.0.1", str(port), name_override, path], capture_output=True, env=env, timeout=40)
    err = r.stderr.decode(errors="replace"); out = r.stdout
    p.kill(); p.wait()
    if expect_fail:
        ok = r.returncode != 0 and expect_fail in err
    else:
        needle = f"{want_ver} {want_suite}" + (f" {want_group}" if want_group else "")
        ok = r.returncode == 0 and needle in err and b"200" in out.split(b"\r\n")[0] or False
        if ok and not insecure: ok = "verify=0" in err
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

# ---- certificate validation (x509.c + tls.c) ----
run("cert chain validates for an RSA server", ["-tls1_3"], "rsa", "TLSv1.3", "TLS_", "")
run("cert chain validates for an ECDSA server", ["-tls1_3"], "ec", "TLSv1.3", "TLS_", "")
run("cert chain validates for an ECDSA server", ["-tls1_3"], "ec", "TLSv1.3", "TLS_", "")
run("cert chain validates for an ECDSA P-384 server", ["-tls1_3"], "ec384", "TLSv1.3", "TLS_", "")
run("TLS1.2 certificate validates (SKE signature)", ["-tls1_2"], "rsa", "TLSv1.2", "ECDHE-RSA-", "")
run("TLS1.2 ECDSA certificate validates (SKE signature)", ["-tls1_2"], "ec", "TLSv1.2", "ECDHE-ECDSA-", "")
# No TLS 1.2 P-384 SKE case: OpenSSL binds an ECDSA P-384 certificate to the secp384r1 group, which we deliberately
# don't offer for ECDHE (x25519 + P-256 only), so the server answers "no shared cipher". P-384 signatures are still
# covered by the TLS 1.3 case above and by the X.509 differential tests.
run("unknown CA is refused", ["-tls1_3"], "rsa", "", "", "", ca=None, expect_fail="TLS_FAIL -17")
run("wrong CA is refused", ["-tls1_3"], "rsa", "", "", "", ca=f"{tmp}/wrong.crt", expect_fail="TLS_FAIL -17")
run("hostname mismatch is refused", ["-tls1_3"], "rsa", "", "", "", name_override="wrong.example", expect_fail="TLS_FAIL -17")
run("hostname mismatch is refused on TLS 1.2 too", ["-tls1_2"], "rsa", "", "", "", name_override="wrong.example", expect_fail="TLS_FAIL -17")
run("--insecure accepts an untrusted certificate", ["-tls1_3"], "rsa", "TLSv1.3", "TLS_", "", ca=None, insecure=True)
run("--insecure accepts a wrong hostname", ["-tls1_3"], "rsa", "TLSv1.3", "TLS_", "", ca=None, insecure=True, name_override="wrong.example")
run("chain of 3 certs validates against the root (1.3)", ["-tls1_3"], "leaf", "TLSv1.3", "TLS_", "", chain="inter", ca=f"{tmp}/root.crt")
run("chain validates against the intermediate (1.3)", ["-tls1_3"], "leaf", "TLSv1.3", "TLS_", "", chain="inter", ca=f"{tmp}/inter.crt")
run("chain validates and SKE signature checks (1.2)", ["-tls1_2"], "leaf", "TLSv1.2", "ECDHE-RSA-", "", chain="inter", ca=f"{tmp}/root.crt")
run("leaf alone is not a trust anchor", ["-tls1_3"], "leaf", "", "", "", chain="inter", ca=f"{tmp}/leaf.crt", expect_fail="TLS_FAIL -17")

for p in procs:
    try: p.kill()
    except Exception: pass
shutil.rmtree(tmp, ignore_errors=True)
print("ALL OK" if not fails else f"{fails} FAILED"); sys.exit(1 if fails else 0)
