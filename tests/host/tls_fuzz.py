#!/usr/bin/env python3
"""Mutation fuzzer for the TLS client: a proxy corrupts / truncates / duplicates bytes of the REAL server flight
(random bit flips, byte overwrites, truncation, garbage injection) and the (sanitizer-built) client must never
crash, hang, or leak -- it must always end with a clean error or (if the corruption was harmless) success.
usage: tls_fuzz.py /path/to/tls_host_asan [iterations]"""
import subprocess, sys, os, time, socket, threading, random, tempfile, shutil, select
host = sys.argv[1]; N = int(sys.argv[2]) if len(sys.argv) > 2 else 150
tmp = tempfile.mkdtemp(prefix="tlsf")
for kind, args in (("rsa", ["rsa:2048"]), ("ec", ["ec", "-pkeyopt", "ec_paramgen_curve:prime256v1"])):
    subprocess.run(["openssl", "req", "-x509", "-newkey", *args, "-nodes", "-keyout", f"{tmp}/{kind}.key", "-out", f"{tmp}/{kind}.crt", "-days", "2", "-subj", "/CN=localhost"], check=True, capture_output=True)
CA = f"{tmp}/ca.pem"                                   # trust both certs: unmutated flights verify, mutated ones fail cleanly
with open(CA, "wb") as f:
    for c in ("rsa", "ec"): f.write(open(f"{tmp}/{c}.crt", "rb").read())
env = dict(os.environ, TLS_CAFILE=CA)
rnd = random.Random(2026)
servers = {}
def server(name, extra, cert, port):
    p = subprocess.Popen(["openssl", "s_server", "-accept", str(port), "-cert", f"{tmp}/{cert}.crt", "-key", f"{tmp}/{cert}.key", "-www", "-quiet"] + extra,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.PIPE)
    servers[name] = (p, port)
server("t13", ["-tls1_3"], "rsa", 27001)
server("t13aes", ["-tls1_3", "-ciphersuites", "TLS_AES_256_GCM_SHA384", "-groups", "P-256"], "ec", 27002)
server("t12", ["-tls1_2", "-cipher", "ECDHE-RSA-AES128-GCM-SHA256"], "rsa", 27003)
server("t12cc", ["-tls1_2", "-cipher", "ECDHE-ECDSA-CHACHA20-POLY1305", "-groups", "P-256"], "ec", 27004)
time.sleep(0.6)

def mutate(data, mode):
    d = bytearray(data)
    if not d: return bytes(d)
    if mode == 0:                                   # flip a few bits
        for _ in range(rnd.randint(1, 4)): d[rnd.randrange(len(d))] ^= 1 << rnd.randrange(8)
    elif mode == 1:                                 # overwrite bytes
        for _ in range(rnd.randint(1, 6)): d[rnd.randrange(len(d))] = rnd.randrange(256)
    elif mode == 2:                                 # truncate
        d = d[:rnd.randrange(len(d))]
    elif mode == 3:                                 # corrupt the record/handshake headers specifically
        for off in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9):
            if off < len(d) and rnd.random() < 0.3: d[off] = rnd.randrange(256)
    elif mode == 4:                                 # insert garbage
        i = rnd.randrange(len(d)); d[i:i] = bytes(rnd.randrange(256) for _ in range(rnd.randint(1, 40)))
    elif mode == 5:                                 # duplicate a chunk
        i = rnd.randrange(len(d)); j = min(len(d), i + rnd.randint(1, 200)); d[i:i] = d[i:j]
    return bytes(d)

def proxy_once(up_port, mode, cut):
    ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); ls.bind(("127.0.0.1", 0)); ls.listen(1)
    port = ls.getsockname()[1]
    def run():
        try:
            c, _ = ls.accept(); s = socket.create_connection(("127.0.0.1", up_port)); sent = 0; mutated = False
            c.settimeout(0.5)
            while True:
                r, _, _ = select.select([c, s], [], [], 1.5)
                if not r: break
                if c in r:
                    d = c.recv(65536)
                    if not d: break
                    s.sendall(d)
                if s in r:
                    d = s.recv(65536)
                    if not d: break
                    if not mutated and sent + len(d) > cut:       # mutate the chunk in which the cut point falls
                        d = mutate(d, mode); mutated = True
                    sent += len(d)
                    try: c.sendall(d)
                    except Exception: break
        except Exception: pass
        finally:
            try: c.close()
            except Exception: pass
            try: s.close()
            except Exception: pass
            ls.close()
    t = threading.Thread(target=run, daemon=True); t.start()
    return port

stats = {"ok": 0, "clean_error": 0}; bad = 0
for i in range(N):
    name = rnd.choice(list(servers)); up = servers[name][1]
    mode = rnd.randrange(6); cut = rnd.choice([0, 0, 5, 40, 100, 300, 800, 1500, 3000])
    port = proxy_once(up, mode, cut)
    try:
        r = subprocess.run([host, "127.0.0.1", str(port), "localhost", "/"], capture_output=True, timeout=25, env=env)
    except subprocess.TimeoutExpired:
        bad += 1; print(f"HANG   server={name} mode={mode} cut={cut}"); continue
    err = r.stderr.decode(errors="replace")
    if r.returncode in (0, 2, 3, 4, 5) and "ERROR: AddressSanitizer" not in err and "runtime error" not in err:
        stats["ok" if r.returncode == 0 else "clean_error"] += 1
    else:
        bad += 1; print(f"CRASH  server={name} mode={mode} cut={cut} rc={r.returncode}\n" + err[-700:])
for p, _ in servers.values(): p.kill()
shutil.rmtree(tmp, ignore_errors=True)
print(f"{N} mutated sessions: {stats['clean_error']} rejected cleanly, {stats['ok']} unaffected, {bad} crashes/hangs")
sys.exit(1 if bad else 0)
