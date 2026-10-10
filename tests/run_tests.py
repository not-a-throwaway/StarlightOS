#!/usr/bin/env python3
"""
StarlightOS end-to-end tests. Boots the real disk image in QEMU (headless), drives the shell through the emulated
keyboard and checks serial output, files written to the disk image, host-side servers, and captured audio.

    python3 tests/run_tests.py            # everything
    python3 tests/run_tests.py net sound  # only groups whose name matches

Requires: qemu-system-x86_64, python3. Audio checks need numpy (skipped otherwise).
"""
import base64, http.server, os, re, socket, socketserver, struct, subprocess, sys, tempfile, threading, time, urllib.request, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from harness import VM, Ext2Image, PROMPT

DISK = os.path.join(ROOT, 'disk.img')
results = []

def norm(s):
    """Collapse runs of whitespace; ls -l column pads make 'root  root' vary."""
    return re.sub(r'\s+', ' ', s)


def check(name, cond, detail=''):
    results.append((name, bool(cond)))
    print(('  PASS  ' if cond else '  FAIL  ') + name + ('' if cond else f'   <- {detail[:300]!r}'))
    return cond


# --------------------------------------------------------------------- host-side fixture servers
def chunked_bytes(n):
    return bytes((i * 7 + 3) % 251 for i in range(n))

def big_bytes(n):
    return bytes((i * 13) % 256 for i in range(n))

# ---- M6 fixtures: a small RGBA PNG reused as an <img> source and as /pic.png --
def _png_chunk(typ, data):
    return (struct.pack('>I', len(data)) + typ + data +
            struct.pack('>I', zlib.crc32(typ + data) & 0xffffffff))

def png_bytes(w, h, rows_rgba):
    raw = b''.join(b'\x00' + bytes(r) for r in rows_rgba)
    return (b'\x89PNG\r\n\x1a\n' +
            _png_chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
            _png_chunk(b'IDAT', zlib.compress(raw)) +
            _png_chunk(b'IEND', b''))

PNG_SMALL = png_bytes(2, 2, [[255, 0, 0, 255, 0, 255, 0, 255],
                             [0, 0, 255, 255, 255, 255, 255, 128]])

class HTTP(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass
    def send(self, code, body, ctype='text/plain', extra=None, chunked=False):
        self.send_response(code); self.send_header('Content-Type', ctype)
        for k, v in (extra or {}).items(): self.send_header(k, v)
        if chunked:
            self.send_header('Transfer-Encoding', 'chunked'); self.send_header('Connection', 'close'); self.end_headers()
            for i in range(0, len(body), 700):
                c = body[i:i + 700]; self.wfile.write(('%x\r\n' % len(c)).encode() + c + b'\r\n')
            self.wfile.write(b'0\r\n\r\n')
        else:
            self.send_header('Content-Length', str(len(body))); self.send_header('Connection', 'close'); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        p = self.path
        if p == '/hello': self.send(200, b'Hello, StarlightOS!\nline two\n')
        elif p == '/redir': self.send(302, b'', extra={'Location': '/hello'})
        elif p == '/redir2': self.send(301, b'', extra={'Location': 'http://10.0.2.2:18001/redir'})
        elif p == '/loop': self.send(302, b'', extra={'Location': '/loop'})
        elif p == '/chunked': self.send(200, chunked_bytes(5000), 'application/octet-stream', chunked=True)
        elif p == '/big': self.send(200, big_bytes(300000), 'application/octet-stream')
        elif p == '/https': self.send(302, b'', extra={'Location': 'https://example.com/'})
        elif p == '/lines': self.send(200, ''.join('line %03d alpha\n' % i for i in range(120)).encode())
        elif p == '/page':
            self.send(200, b'<html><head><title>T</title><style>p{color:red}</style></head><body><h1>Big Title</h1>'
                      b'<p>Some <b>bold</b> text &amp; an entity here.</p><ul><li>one</li><li>two</li></ul>'
                      b'<a href="/hello">a link</a><script>var hidden=1;</script></body></html>', 'text/html')
        elif p == '/pic.png':
            self.send(200, PNG_SMALL, 'image/png')
        elif p == '/m6page':
            b64 = base64.b64encode(PNG_SMALL).decode()
            self.send(200, (
                '<html><head><style>\n'
                'h1 { color: #3366ff; }\n'
                'p { color: lime; text-decoration: underline; }\n'
                '.big { font-size: 24px; }\n'
                '</style></head><body>'
                '<h1>Styled Heading</h1>'
                '<p class="big">Underlined big text</p>'
                '<p>inline <img src="data:image/png;base64,' + b64 + '"> then '
                '<img src="/pic.png"> done</p>'
                '</body></html>').encode(), 'text/html')
        elif p == '/head': self.send(200, b'body-should-not-be-printed-by-HEAD', extra={'X-Test': 'yes'})
        elif p == '/form':
            self.send(200, (
                '<html><body>'
                '<h2>Search</h2>'
                '<form action="/echo" method="get">'
                '<input type="text" name="q" value="">'
                '<input type="checkbox" name="c" value="on">'
                '<input type="submit" value="Go">'
                '</form>'
                '</body></html>').encode(), 'text/html')
        elif p.startswith('/echo'): self.send(200, ('echo:' + p).encode())
        else: self.send(404, b'<h1>nope</h1>', 'text/html')
    def do_HEAD(self):
        self.send_response(200); self.send_header('Content-Length', '999'); self.send_header('X-Test', 'yes'); self.end_headers()
    def do_POST(self):
        n = int(self.headers.get('Content-Length', 0)); b = self.rfile.read(n)
        self.send(200, b'got:' + b + b'|' + (self.headers.get('Content-Type') or '').encode())

class Threaded(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True; allow_reuse_address = True

def dns_stub(port, stop):
    def name_at(m, p):
        parts = []
        while m[p]:
            l = m[p]; parts.append(m[p + 1:p + 1 + l].decode()); p += l + 1
        return '.'.join(parts), p + 1
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(('0.0.0.0', port)); s.settimeout(0.3)
    while not stop.is_set():
        try: m, a = s.recvfrom(512)
        except socket.timeout: continue
        name, p = name_at(m, 12); qend = p + 4
        hdr = m[:2]; q = m[12:qend]
        def enc(n): return b''.join(bytes([len(x)]) + x.encode() for x in n.split('.')) + b'\0'
        if name == 'a.test':
            ans = b'\xc0\x0c\x00\x01\x00\x01' + struct.pack('>I', 300) + b'\x00\x04' + bytes([1, 2, 3, 4]); rc, n = 0, 1
        elif name == 'www.test':
            c = enc('a.test')
            ans = b'\xc0\x0c\x00\x05\x00\x01' + struct.pack('>I', 300) + struct.pack('>H', len(c)) + c
            ans += enc('a.test') + b'\x00\x01\x00\x01' + struct.pack('>I', 120) + b'\x00\x04' + bytes([9, 8, 7, 6]); rc, n = 0, 2
        else:
            ans, rc, n = b'', 3, 0
        s.sendto(hdr + struct.pack('>HHHHH', 0x8180 | rc, 1, n, 0, 0) + q + ans, a)

NTP_UNIX = 1907755200   # 2030-06-15 12:00:00 UTC
def ntp_stub(port, stop):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(('0.0.0.0', port)); s.settimeout(0.3)
    while not stop.is_set():
        try: m, a = s.recvfrom(512)
        except socket.timeout: continue
        r = bytearray(48); r[0] = 0x24; r[1] = 2
        ts = NTP_UNIX + 2208988800
        struct.pack_into('>II', r, 40, ts, 0); struct.pack_into('>II', r, 32, ts, 0)
        s.sendto(bytes(r), a)

def echo_server(port, stop):
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); srv.bind(('0.0.0.0', port)); srv.listen(2); srv.settimeout(0.3)
    while not stop.is_set():
        try: c, _ = srv.accept()
        except socket.timeout: continue
        c.sendall(b'WELCOME\n'); c.settimeout(5)
        try:
            data = c.recv(100)
            c.sendall(b'ECHO:' + data)
        except Exception: pass
        c.close()


# --------------------------------------------------------------------------------------- groups
def group_shell(work):
    print('\n[shell, files, text tools]')
    vm = VM(DISK, work)
    try:
        if not check('boots to a shell prompt', vm.boot(), vm.log()[-200:]):
            return
        r = vm.run('echo one two three | wc -w');          check('pipe: echo | wc -w', r.split()[-1] == '3', r)
        r = vm.run('seq 5 | tail -n 2');                   check('pipe: seq | tail', r.split() == ['4', '5'], r)
        r = vm.run('seq 20 | grep 1 | wc -l');              check('pipe chain of three', r.split()[0] == '11', r)
        vm.run('echo hello world > T1.TXT')
        r = vm.run('cat T1.TXT');                           check('redirect > then cat', r == 'hello world', r)
        vm.run('echo second line >> T1.TXT')
        r = vm.run('wc -l T1.TXT');                         check('redirect >> appends', r.split()[0] == '2', r)
        r = vm.run('set A=42; echo $A and ${A}x');          check('variables expand', r == '42 and 42x', r)
        r = vm.run('echo "quoted  spaces" \'single $A\'');  check('quoting', r == 'quoted  spaces single $A', r)
        vm.run('alias hi=echo\\ hello'); r = vm.run('hi');  check('alias', 'hello' in r, r)
        r = vm.run('calc (3+4)*0x10 - 5');                  check('calc', r.startswith('107'), r)
        r = vm.run('calc 1/0 && echo SHOULD_NOT_RUN');      check('&& short-circuits on failure', 'SHOULD_NOT_RUN' not in r and 'division' in r, r)
        r = vm.run('ls | grep -i chime');                   check('ls | grep', 'CHIME.WAV' in r, r)
        r = vm.run('cal 2 2024');                           check('cal (leap February has 29)', '29' in r and 'February 2024' in r, r)
        vm.run('mkdir DIR1'); r = vm.run('cd DIR1; pwd');   check('mkdir + cd + pwd', r.strip() == '/DIR1', r)
        vm.run('echo nested > N.TXT'); r = vm.run('cd /; find N.TXT'); check('find recurses', 'DIR1/N.TXT' in r, r)
        r = vm.run('cd /; tree');                           check('tree lists the new directory', 'DIR1/' in r, r)
        vm.run('rm DIR1/N.TXT'); r = vm.run('rmdir DIR1; ls | grep -c DIR1'); check('rmdir', r.strip().endswith('0'), r)
        # /dev: the devfs device filesystem (devfs.c)
        r = vm.run('ls / | grep -c dev');                  check('/dev is listed at the root', r.strip() == '1', r)
        r = vm.run('cd /dev; pwd');                        check('cd /dev works', r.strip() == '/dev', r)
        r = vm.run('ls');                                  check('devfs lists the device nodes',
                                                            all(d in r.split() for d in ('null', 'zero', 'random', 'urandom', 'tty')), r)
        r = vm.run('cd /; cat /dev/null; echo end=ok');    check('/dev/null: read is end-of-file', r.strip() == 'end=ok', r)
        r = vm.run('echo gone > /dev/null; echo rc=$?');   check('/dev/null: write discarded cleanly', r.strip() == 'rc=0', r)
        r = vm.run('hexdump /dev/zero | head -n 1');       check('/dev/zero: read yields zeroes', '00 00 00 00 00 00 00 00' in r, r)
        r = vm.run('hexdump /dev/urandom | head -n 1');    check('/dev/urandom: read yields random bytes',
                                                            r.startswith('00000000') and '00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00' not in r, r)
        r = vm.run('echo to the console > /dev/tty');      check('/dev/tty: write reaches the console', 'to the console' in r, r)
        r = vm.run('rm /dev/null; echo rc=$?');            check('/dev nodes cannot be deleted', 'operation not permitted' in r and 'rc=1' in r, r)
        r = vm.run('mkdir /dev/x; echo rc=$?');            check('/dev nodes cannot be created', 'operation not permitted' in r and 'rc=1' in r, r)
        # /dev/tty: a read blocks for keyboard input, Ctrl-C ends it (the
        # keyboard IRQ raises the abort flag while the read is parked)
        start = len(vm.log())
        vm.type('cat /dev/tty\n'); time.sleep(1.5)
        vm.type('typed on tty\n'); time.sleep(1.5)
        seg = vm.log()[start:]
        check('/dev/tty: read returns keyboard input', 'typed on tty' in seg and not PROMPT.search(seg), seg[-200:])
        vm.key('ctrl-c'); time.sleep(2.0)
        check('/dev/tty: Ctrl-C ends the read', PROMPT.search(vm.log()[start:]) is not None, vm.log()[start:][-200:])
        r = vm.run('seq 3 | sort -r | head -n 1');          check('sort -r', r.strip() == '3', r)
        r = vm.run('cat nosuchfile; echo status=$?');       check('error status propagates ($?)', 'no such file' in r and 'status=1' in r, r)
        r = vm.run('bogus');                                check('unknown command message', 'command not found' in r, r)
        r = vm.run('hexdump HELLO.TXT | head -n 1');        check('hexdump', '48 65 6c 6c 6f' in r, r)
        r = vm.run('uname -a');                             check('uname', 'StarlightOS' in r and 'x86_64' in r, r)
        r = vm.run('mem');                                  check('mem shows heap', 'Kernel heap' in r, r)
        r = vm.run('lspci');                                check('lspci sees NIC and audio', 'RTL8139' in r and "AC'97" in r, r)
        r = vm.run('which ll');                             check('which alias', "aliased to 'ls -l'" in r, r)
        r = vm.run('help sound');                           check('help GROUP', 'beep' in r and 'wav' in r, r)
        # tab completion + history editing
        vm.type('cat HELLO.T'); vm.key('tab'); time.sleep(0.5); vm.type('\n'); time.sleep(1.2)
        check('tab completion', 'Hello from StarlightOS' in vm.log()[-400:], vm.log()[-300:])
        # cursor-key line editing: type "echo abXd", Left,Left, insert 'c' -> "echo abcXd"? simpler: Home + insert
        vm.type('cho editing ok'); vm.key('home'); vm.type('e'); vm.type('\n'); time.sleep(1.0)
        check('line editing (Home + insert)', 'editing ok' in vm.log()[-200:], vm.log()[-200:])
        vm.key('up'); time.sleep(0.3); vm.type('\n'); time.sleep(1.0)
        check('history (Up recalls last command)', vm.log()[-300:].count('editing ok') >= 3, vm.log()[-300:])
        # scripts
        r = vm.run('source DEMO.SH', timeout=20);           check('script: DEMO.SH runs', 'hello from a script' in r.lower() and 'Audio' in r, r[-300:])
        # big file write speed + integrity (FAT cache regression)
        t = time.time(); vm.run('seq 4000 > SEQ.TXT', timeout=60); dt = time.time() - t
        check('writes a 24 KB file quickly', dt < 25, f'{dt:.1f}s')
    finally:
        vm.quit()
    img = Ext2Image(vm.disk)
    t1 = img.read('T1.TXT'); check('disk image: T1.TXT bytes', t1 == b'hello world\nsecond line\n', repr(t1))
    sq = img.read('SEQ.TXT'); check('disk image: SEQ.TXT integrity', sq == ''.join('%d\n' % i for i in range(1, 4001)).encode(), repr(sq[:60] if sq else sq))


def group_net(work):
    print('\n[network: DHCP ARP ICMP UDP TCP DNS NTP HTTP]')
    stop = threading.Event()
    http_srv = Threaded(('0.0.0.0', 18001), HTTP)
    threads = [threading.Thread(target=http_srv.serve_forever, daemon=True),
               threading.Thread(target=dns_stub, args=(15353, stop), daemon=True),
               threading.Thread(target=ntp_stub, args=(12300, stop), daemon=True),
               threading.Thread(target=echo_server, args=(18002, stop), daemon=True)]
    for t in threads: t.start()
    vm = VM(DISK, work, hostfwd=',hostfwd=tcp::18080-:8080')
    try:
        if not check('boots', vm.boot(), vm.log()[-200:]): return
        check('DHCP lease acquired', vm.wait_lease(), vm.log()[-300:])
        r = vm.run('ip')
        check('ip: address/gateway/dns from DHCP', '10.0.2.15' in r and 'gateway  10.0.2.2' in r and 'dns      10.0.2.3' in r, r)
        r = vm.run('ping -c 3 10.0.2.2', timeout=20)
        check('ping gateway 3/3', '3 sent, 3 received' in r, r)
        r = vm.run('arp'); check('ARP table has gateway', '10.0.2.2' in r, r)
        r = vm.run('ping -c 1 -t 1 10.0.2.3', timeout=10)
        check('ping with TTL 1 / unreachable handled', 'ping statistics' in r, r)
        r = vm.run('traceroute 10.0.2.2', timeout=20); check('traceroute reaches 1-hop gateway', ' 1  10.0.2.2' in r, r)

        r = vm.run('fetch http://10.0.2.2:18001/hello'); check('fetch: GET body + status line', '200 OK' in r and 'Hello, StarlightOS!' in r and 'line two' in r, r)
        r = vm.run('fetch http://10.0.2.2:18001/redir'); check('fetch: follows 302', 'redirect to http://10.0.2.2:18001/hello' in r and 'Hello, StarlightOS!' in r, r)
        r = vm.run('fetch http://10.0.2.2:18001/redir2', timeout=20); check('fetch: follows absolute 301 -> 302 chain', r.count('redirect to') == 2 and 'Hello, StarlightOS!' in r, r)
        r = vm.run('fetch --no-redirect http://10.0.2.2:18001/redir'); check('fetch: --no-redirect keeps 302', '302' in r and 'Hello, StarlightOS!' not in r, r)
        r = vm.run('fetch http://10.0.2.2:18001/loop', timeout=25); check('fetch: redirect loop is cut off', 'too many redirects' in r, r)
        r = vm.run('fetch http://10.0.2.2:18001/nothing; echo rc=$?'); check('fetch: 404 -> status + nonzero exit', '404' in r and 'rc=1' in r, r)
        r = vm.run('fetch -i http://10.0.2.2:18001/hello'); check('fetch -i prints headers', 'Content-Type: text/plain' in r and 'Content-Length: 29' in r, r)
        r = vm.run('fetch -I http://10.0.2.2:18001/head'); check('fetch -I (HEAD) prints headers, no body', 'X-Test: yes' in r, r)
        r = vm.run('fetch -d "a=1&b=2" http://10.0.2.2:18001/post'); check('fetch -d POSTs data with content-type', 'got:a=1&b=2|application/x-www-form-urlencoded' in r, r)
        r = vm.run('fetch http://10.0.2.2:18001/page'); check('fetch renders HTML as text', 'Big Title' in r and 'bold' in r and '&amp;' not in r and '<h1>' not in r and 'hidden' not in r and '[1]' in r, r)
        r = vm.run('fetch -r http://10.0.2.2:18001/page'); check('fetch -r prints raw HTML', '<h1>Big Title</h1>' in r, r)
        r = vm.run('fetch http://10.0.2.2:18001/https'); check('https is refused clearly (no TLS)', 'not supported' in r or 'no TLS' in r, r)
        r = vm.run('fetch http://10.0.2.2:18001/lines | grep -c alpha', timeout=20); check('fetch | grep pipeline (raw body in pipe)', r.strip().split()[-1] == '120', r)
        r = vm.run('fetch http://10.0.2.2:18001/chunked -o CH.BIN', timeout=20); check('fetch -o chunked save', 'Saved 5000 bytes' in r, r)
        r = vm.run('fetch http://10.0.2.2:18001/big -o BIG.BIN', timeout=90); check('fetch -o 300 KB save', 'Saved 300000 bytes' in r, r)
        r = vm.run('fetch -O http://10.0.2.2:18001/hello'); check('fetch -O names file from URL', 'Saved 29 bytes to HELLO' in r, r)
        r = vm.run('fetch http://10.0.2.2:1/', timeout=20); check('connection refused reported', 'connection failed' in r, r)

        r = vm.run('nslookup a.test 10.0.2.2:15353'); check('DNS A record via custom server', 'Address: 1.2.3.4' in r, r)
        r = vm.run('nslookup www.test 10.0.2.2:15353'); check('DNS CNAME chain parsed (compressed names)', 'alias for a.test' in r and 'Address: 9.8.7.6' in r, r)
        r = vm.run('nslookup nx.test 10.0.2.2:15353'); check('DNS NXDOMAIN reported', 'NXDOMAIN' in r, r)
        r = vm.run('ntpdate 10.0.2.2:12300'); check('NTP query parses reply', 'UTC time: 2030-06-15 12:00:0' in r and 'stratum 2' in r, r)
        r = vm.run('ntpdate -s 10.0.2.2:12300; date'); check('ntpdate -s sets the RTC', '2030' in r and 'Jun' in r, r)

        # nc client against the host echo server
        vm.type('nc 10.0.2.2 18002\n'); vm.wait_for('Connected', 10, len(vm.log()) - 100)
        time.sleep(1.0); vm.type('ping-pong\n'); time.sleep(2.0)
        lg = vm.log()[-400:]
        check('nc: connects, receives banner and echo', 'WELCOME' in lg and 'ECHO:ping-pong' in lg, lg)
        vm.key('ctrl-c'); time.sleep(1.0)

        # httpd: serve the disk to the host via hostfwd
        start = len(vm.log())
        vm.type('httpd 8080\n'); ok = vm.wait_for('Serving', 10, start)
        check('httpd starts', ok, vm.log()[start:])
        if ok:
            def get(path):
                try:
                    with urllib.request.urlopen('http://127.0.0.1:18080' + path, timeout=10) as rsp: return rsp.status, rsp.read()
                except urllib.error.HTTPError as e: return e.code, e.read()
                except Exception as e: return -1, repr(e).encode()
            st, body = get('/'); check('httpd: / serves INDEX.HTM', st == 200 and b'Hello from StarlightOS' in body, repr((st, body[:80])))
            st, body = get('/HELLO.TXT'); check('httpd: serves a text file', st == 200 and b'Hello from StarlightOS!' in body, repr((st, body[:80])))
            st, body = get('/CHIME.WAV'); check('httpd: serves a binary (WAV) file intact', st == 200 and body[:4] == b'RIFF' and len(body) > 30000, repr((st, len(body))))
            st, body = get('/MISSING.TXT'); check('httpd: 404', st == 404, repr((st, body[:40])))
            vm.key('ctrl-c')
            check('httpd stops on Ctrl-C', vm.wait_for('httpd stopped', 10, start), vm.log()[start:][-300:])
            check('httpd logged requests', 'GET /HELLO.TXT -> 200' in vm.log()[start:], vm.log()[start:][-400:])
        time.sleep(1.5)
        r = vm.run('netstat -s'); check('netstat counters', 'TCP   rx' in r and 'retransmits' in r and 'window-scaled' in r, r)
        r = vm.run('netstat'); check('netstat table has a WS column', 'WS' in r and ('tcp' in r or 'Proto' in r), r)
    finally:
        vm.quit(); stop.set(); http_srv.shutdown()
    img = Ext2Image(vm.disk)
    ch = img.read('CH.BIN'); check('disk image: chunked download is byte-exact', ch == chunked_bytes(5000), f'{None if ch is None else len(ch)}')
    big = img.read('BIG.BIN'); check('disk image: 300 KB download is byte-exact', big == big_bytes(300000), f'{None if big is None else len(big)}')


def group_sound(work):
    print('\n[sound: mixer, MML, WAV, stereo, render]')
    try:
        import numpy as np
        sys.path.insert(0, HERE)
        import wavanalyze as W
    except ImportError:
        print('  SKIP  numpy not installed'); return
    vm = VM(DISK, work, audio=True)
    marks = {}
    try:
        if not check('boots', vm.boot(), vm.log()[-200:]): return
        time.sleep(2.5)                                    # let the startup chime finish
        r = vm.run('sound'); check('sound status shows AC97', "AC'97" in r and '48000 Hz' in r, r)
        def timed(label, cmd, timeout=15):
            marks[label] = [time.time()]
            out = vm.run(cmd, timeout=timeout); marks[label].append(time.time()); time.sleep(0.8); return out
        vm.run('vol 100')
        t_base = time.time()
        timed('sine', 'tone 880 1200 sine 90')
        timed('square', 'tone 440 1000 square 90')
        timed('mml', 'play -w sine "t240 o5 l8 c e g"')
        timed('stereo', 'wav STEREO.WAV')
        timed('chime', 'wav CHIME.WAV')
        timed('tone8', 'wav TONE8.WAV')
        r = vm.run('synth OUT.WAV -w sine "o5 l4 a"', timeout=20); check('synth renders a WAV file', 'done' in r, r)
        timed('rendered', 'wav OUT.WAV')
        timed('quiet', 'tone 880 800 sine 90')              # measured after vol change below
        vm.run('vol 25'); timed('quiet', 'tone 880 800 sine 90')
        r = vm.run('play "c d e x"'); check('MML syntax error reports position', 'syntax error at character 7' in r, r)
        r = vm.run('wav NOPE.WAV'); check('missing WAV reported', 'file not found' in r, r)
        r = vm.run('wav HELLO.TXT'); check('non-WAV rejected', 'not a RIFF/WAVE' in r, r)
        r = vm.run('sound stop; sound'); check('sound status after stop', '0 active' in r, r)
        r = vm.run('vol 55; vol'); check('vol set/get', '55%' in r, r)
        time.sleep(1.0)
    finally:
        vm.quit()
    a, rate = W.load(vm.audio_path)
    print(f'  (captured {len(a)/rate:.1f}s of audio at {rate} Hz, peak {abs(a).max():.2f})')
    # Capture timeline starts at VM start; use wall-clock marks relative to the first mark minus startup offset.
    # We locate each sound by energy instead of trusting offsets: find contiguous loud regions.
    mono = a.mean(axis=1)
    env = np.convolve(np.abs(mono), np.ones(int(rate * 0.02)) / (rate * 0.02), mode='same')
    loud = env > 0.01
    regions = []; i = 0
    while i < len(loud):
        if loud[i]:
            j = i
            while j < len(loud) and (loud[j] or (j + int(rate * 0.12) < len(loud) and loud[j:j + int(rate * 0.12)].any())): j += 1
            regions.append((i / rate, j / rate)); i = j
        else: i += 1
    regions = [(s, e) for s, e in regions if e - s > 0.15]
    def dom(seg, ch=None):
        x = seg.mean(axis=1) if ch is None else seg[:, ch]
        sp = np.abs(np.fft.rfft(x * np.hanning(len(x)))); f = np.fft.rfftfreq(len(x), 1 / rate); return f[sp.argmax()]
    def region_for(k): return regions[k] if k < len(regions) else (0, 0)
    # region 0 = startup chime (C E G C), then sine, square, mml, stereo, chime, tone8, rendered, loud, quiet
    check('captured expected number of sound events', len(regions) >= 9, str([(round(s, 1), round(e, 1)) for s, e in regions]))
    if len(regions) >= 9:
        def seg(k, trim=0.1):
            s, e = region_for(k); return a[int((s + trim) * rate):int((e - trim) * rate)]
        d = dom(seg(1)); check('tone 880 Hz sine is on pitch', abs(d - 880) < 12, f'{d:.1f}')
        s, e = region_for(1); check('tone 880 Hz lasts ~1.2 s', 1.0 < e - s < 1.7, f'{e - s:.2f}')
        d = dom(seg(2)); check('tone 440 Hz square is on pitch', abs(d - 440) < 8, f'{d:.1f}')
        # square wave has strong odd harmonics, sine does not
        x = seg(2).mean(axis=1); sp = np.abs(np.fft.rfft(x * np.hanning(len(x)))); f = np.fft.rfftfreq(len(x), 1 / rate)
        h3 = sp[np.argmin(abs(f - 1320))] / sp[np.argmin(abs(f - 440))]
        check('square wave has odd harmonics (waveform differs from sine)', 0.2 < h3 < 0.5, f'{h3:.2f}')
        m = region_for(3); mm = a[int(m[0] * rate):int(m[1] * rate)]
        thirds = [dom(mm[int(len(mm) * (i + 0.2) / 3):int(len(mm) * (i + 0.8) / 3)]) for i in range(3)]
        check('MML "c e g" at octave 5 -> 523/659/784 Hz', all(abs(t - w) < 18 for t, w in zip(thirds, (523, 659, 784))), str([round(t) for t in thirds]))
        st = seg(4, 0.25)
        dl, dr = dom(st, 0), dom(st, 1)
        check('stereo WAV keeps channels separate (L 440, R 660)', abs(dl - 440) < 10 and abs(dr - 660) < 10, f'{dl:.0f}/{dr:.0f}')
        ch = region_for(5); cm = a[int(ch[0] * rate):int(ch[1] * rate)]
        q = len(cm) // 5
        notes = [dom(cm[int(q * 0.0 + 0.0): q]), dom(cm[q:2 * q]), dom(cm[2 * q:3 * q]), dom(cm[3 * q + q // 2:])]
        check('22.05 kHz WAV is resampled with correct pitch (C5 E5 G5 C6)', abs(notes[-1] - 1047) < 25 and abs(notes[0] - 523) < 20, str([round(n) for n in notes]))
        d = dom(seg(6)); check('8-bit 8 kHz WAV plays at 1000 Hz', abs(d - 1000) < 15, f'{d:.1f}')
        d = dom(seg(7)); check('synth-rendered WAV (24 kHz mono) plays back at A5 = 880 Hz', abs(d - 880) < 12, f'{d:.1f}')
        loud_rms = np.sqrt((seg(8).mean(axis=1) ** 2).mean()); quiet_rms = np.sqrt((seg(9).mean(axis=1) ** 2).mean())
        ratio = quiet_rms / loud_rms
        check('master volume scales amplitude (25% vs 100% ~ 1:16)', 0.03 < ratio < 0.12, f'{ratio:.3f}')
        clicks = []
        for k in (1, 8):
            x = seg(k, 0.15).mean(axis=1); dx = np.abs(np.diff(x)); clicks.append(dx.max())
        check('no clicks/glitches inside sine tones (max sample step small)', max(clicks) < 0.09, str([round(float(c), 3) for c in clicks]))
    check('output never clips', abs(a).max() <= 1.0, f'{abs(a).max():.3f}')
    img = Ext2Image(vm.disk)
    wavb = img.read('OUT.WAV')
    check('disk image: rendered WAV is a valid 16-bit mono 24 kHz file', bool(wavb) and wavb[:4] == b'RIFF' and struct.unpack_from('<I', wavb, 24)[0] == 24000 and struct.unpack_from('<H', wavb, 34)[0] == 16, repr(wavb[:44] if wavb else wavb))


def group_unix(work):
    print('\n[unix: permissions, FHS, POSIX shell, classic utilities]')
    vm = VM(DISK, work)
    try:
        if not check('boots to a Unix-style shell', vm.boot(), vm.log()[-200:]):
            return

        # --- identity and login environment ---
        r = vm.run('id')
        check('id: euid/gid 1000 named guest', 'uid=1000(guest) gid=1000(guest)' in r, r)
        r = vm.run('whoami')
        check('whoami prints guest', r.strip() == 'guest', r)
        r = vm.run('echo $USER $HOME $PATH')
        check('USER/HOME/PATH are set', r.split() == ['guest', '/home/guest', '/bin:/usr/bin'], r)
        r = vm.run('echo $GROUP $HOME_ALIAS')
        check('/etc/profile and ~/.profile ran at login', r.split() == ['users', 'set'], r)
        r = vm.run('env')
        check('env prints the login environment',
              'HOME=/home/guest' in r and 'PATH=/bin:/usr/bin' in r and 'GROUP=users' in r
              and 'HOME_ALIAS=set' in r and 'USER=guest' in r, r)
        vm.run('export CC=gcc-16')
        r = vm.run('echo $CC')
        check('export NAME=VALUE sets a variable', r.strip() == 'gcc-16', r)
        r = vm.run('export | grep CC')
        check('export lists exported variables', 'export CC=gcc-16' in r, r)

        # --- POSIX shell features ---
        r = vm.run('echo $((2+3*4))')
        check('$(( )) arithmetic with precedence', r.strip() == '14', r)
        r = vm.run('echo $((6/2+1))')
        check('$(( )) division', r.strip() == '4', r)
        r = vm.run('echo $((101%7))')
        check('$(( )) modulo', r.strip() == '3', r)
        r = vm.run('test 5 -gt 3 && echo yes')
        check('test -gt', r.strip() == 'yes', r)
        r = vm.run('[ abc = abc ] && echo eq')
        check('[ ... ] synonym works', r.strip() == 'eq', r)
        r = vm.run('test -d /home && echo dir')
        check('test -d', r.strip() == 'dir', r)
        r = vm.run('test -f HELLO.TXT && echo file')
        check('test -f', r.strip() == 'file', r)
        r = vm.run('test ! -e /nonexistent && echo nx')
        check('test ! -e', r.strip() == 'nx', r)
        r = vm.run('[ -z "" ] && echo empty')
        check('[ -z ]', r.strip() == 'empty', r)

        # --- tilde, HOME and the prompt ---
        r = vm.run('echo ~')
        check('tilde expands to $HOME', r.strip() == '/home/guest', r)
        r = vm.run('cd ~; pwd')
        check('cd ~ enters HOME', r.strip() == '/home/guest', r)
        r = vm.run('cd /; pwd')
        check('returns to /', r.strip() == '/', r)
        r = vm.run('cd; pwd')
        check('bare cd goes to $HOME', r.strip() == '/home/guest', r)
        r = vm.run('cd /; pwd')
        check('back at / again', r.strip() == '/', r)

        # --- /etc and /proc ---
        r = vm.run('cat /etc/hostname')
        check('/etc/hostname = starlight', r.strip() == 'starlight', r)
        r = vm.run('cat /etc/passwd')
        check('/etc/passwd: root and guest accounts', 'root:x:0:0:' in r and 'guest:x:1000:1000:' in r, r)
        r = vm.run('cat /proc/version')
        check('/proc/version', 'StarlightOS' in r and 'x86_64' in r, r)
        r = vm.run('cat /proc/hostname')
        check('/proc/hostname', r.strip() == 'starlight', r)
        r = vm.run('head -n 1 /proc/meminfo')
        check('/proc/meminfo opens', r.startswith('MemTotal:'), r)
        r = vm.run('ls /proc')
        check('/proc lists kernel files', 'version' in r.split() and 'hostname' in r.split(), r)

        # --- ls -l: mode, owner, group, size ---
        r = vm.run('cd / && ls -l | grep HELLO.TXT')
        check('ls -l: perms + owner + size for a file',
              '-rw-r--r--' in r and 'guest guest' in r and '58' in r and 'HELLO.TXT' in r, r)
        r = vm.run('cd / && ls -l | grep MINES.ELF')
        check('ls -l: executable bits show', '-rwxr-xr-x' in r and 'MINES.ELF' in r, r)
        r = vm.run('cd / && ls -l | grep tmp')
        check('ls -l: /tmp sticky 1777 root-owned', 'drwxrwxrwt' in r and 'root root' in norm(r), r)
        r = vm.run('cd / && ls -l | grep home')
        check('ls -l: /home sticky 1777 root-owned', 'drwxrwxrwt' in r and 'root root' in norm(r), r)
        r = vm.run('cd / && ls -l | grep etc')
        check('ls -l: /etc root-owned 0755', 'drwxr-xr-x' in r and 'root root' in norm(r), r)
        r = vm.run('cd / && ls -l | grep -c bin')
        check('ls -l: /bin present', r.strip() == '1', r)
        r = vm.run('ls -l /home')
        check('ls -l: /home/guest is guest-owned', 'drwxr-xr-x' in r and 'guest guest' in norm(r), r)
        r = vm.run('ls -l /dev')
        check('ls -l: devfs nodes are root-owned devices', 'crw-rw-rw-' in r and 'root root' in norm(r), r)

        # --- FHS write restrictions ---
        r = vm.run('mkdir /bin/x; echo rc=$?')
        check('guest cannot create dirs in /bin', 'operation not permitted' in r and 'rc=1' in r, r)
        r = vm.run('echo x > /etc/passwd; echo rc=$?')
        check('guest cannot overwrite /etc/passwd', 'rc=1' in r, r)
        r = vm.run('chmod 644 /etc/passwd; echo rc=$?')
        check('guest cannot chmod root-owned files', 'operation not permitted' in r and 'rc=1' in r, r)
        r = vm.run('touch /tmp/ok.txt; echo rc=$?')
        check('guest can write the sticky /tmp', 'rc=0' in r, r)

        # --- umask ---
        r = vm.run('umask')
        check('umask defaults to 0022', r.strip() == '0022', r)
        vm.run('cd / && umask 077 && touch UM.TXT && umask 022')
        r = vm.run('cd / && ls -l | grep UM.TXT')
        check('umask 077 makes new files 0600', '-rw-------' in r and 'guest guest' in r, r)

        # --- chmod/chown on owned files ---
        vm.run('cd / && echo x > MOD.TXT && chmod 600 MOD.TXT')
        r = vm.run('cd / && ls -l | grep MOD.TXT')
        check('chmod 600', '-rw-------' in r, r)
        vm.run('chmod u+x MOD.TXT')
        r = vm.run('cd / && ls -l | grep MOD.TXT')
        check('chmod u+x', '-rwx------' in r, r)
        vm.run('chmod 755 MOD.TXT')
        r = vm.run('cd / && ls -l | grep MOD.TXT')
        check('chmod 755 octal', '-rwxr-xr-x' in r, r)
        vm.run('cd / && echo x > OW.TXT && chown 0:0 OW.TXT')
        r = vm.run('cd / && ls -l | grep OW.TXT')
        check('chown sets owner:group on disk', 'root root' in norm(r), r)
        r = vm.run('chown 1000:1000 OW.TXT; echo rc=$?')
        check('chown by non-root owner is refused', 'operation not permitted' in r and 'rc=1' in r, r)

        # --- du / stat ---
        r = vm.run('du -s /home/guest')
        check('du -s prints a block count', len(r.split()) == 2 and r.split()[1] == '/home/guest'
              and r.split()[0].isdigit(), r)
        r = vm.run('stat HELLO.TXT')
        check('stat: size/owner/group/mode',
              'Size: 58' in r and 'Owner: guest' in r and 'Group: guest' in r
              and 'Mode: 0644' in r and 'regular file' in r, r)

        # --- classic utilities ---
        r = vm.run("printf '%04d-%s\\n' 7 abc")
        check('printf %04d and %s', r.strip() == '0007-abc', r)
        r = vm.run("printf '%x %o\\n' 255 8")
        check('printf %x and %o', r.strip() == 'ff 10', r)
        r = vm.run('echo abc | tee /tmp/tee.txt && cat /tmp/tee.txt')
        check('tee copies pipe to stdout and file', r.split() == ['abc', 'abc'], r)
        r = vm.run('echo hello | tr a-z A-Z')
        check('tr translates ranges', r.strip() == 'HELLO', r)
        r = vm.run('echo aabbcc | tr -d b')
        check('tr -d deletes', r.strip() == 'aacc', r)
        r = vm.run('echo abcdef | cut -c 2-4')
        check('cut -c selects columns', r.strip() == 'bcd', r)
        r = vm.run('head -n 1 /etc/passwd | cut -d : -f 1')
        check('cut -d -f picks fields', r.strip() == 'root', r)
        r = vm.run("printf '1\\n2\\n3\\n' | xargs echo n")
        check('xargs feeds words to a command', r.strip() == 'n 1 2 3', r)
        r = vm.run('expr 2 + 3')
        check('expr adds', r.strip() == '5', r)
        r = vm.run('expr 20 / 7')
        check('expr divides', r.strip() == '2', r)
        r = vm.run('expr length hello')
        check('expr length', r.strip() == '5', r)
        r = vm.run('basename /a/b/file.txt')
        check('basename', r.strip() == 'file.txt', r)
        r = vm.run('basename /a/b/file.txt .txt')
        check('basename strips suffix', r.strip() == 'file', r)
        r = vm.run('dirname /a/b/file.txt')
        check('dirname', r.strip() == '/a/b', r)
        r = vm.run('dirname foo')
        check('dirname single component', r.strip() == '.', r)
        r = vm.run('yes | head -n 2')
        check('yes | head -n 2 -> y/y', r.split() == ['y', 'y'], r)
        r = vm.run('yes bye | head -n 1')
        check('yes with a string', r.strip() == 'bye', r)

        # --- $PATH script execution ---
        r = vm.run('greet')
        check('$PATH finds /bin/greet script', 'Hello from /bin/greet' in r, r)
        r = vm.run('up')
        check('$PATH finds /usr/bin/up script', 'up from /usr/bin' in r, r)
        r = vm.run('ls -l /bin | grep greet')
        check('/bin/greet is root-owned executable', '-rwxr-xr-x' in r and 'root root' in norm(r), r)

        # --- symlinks ---
        r = vm.run('cd /home/guest && echo hello > F.TXT && ln -s F.TXT LNK && cat LNK')
        check('ln -s then cat resolves the link', r.strip() == 'hello', r)
        r = vm.run('cd /home/guest && ls -l | grep LNK')
        check('ls -l shows a symlink with its target', 'lrwxrwxrwx' in r and 'LNK -> F.TXT' in r, r)
        r = vm.run('cd /home/guest && stat LNK')
        check('stat reports a symbolic link', 'Type: symbolic link' in r and 'Target: F.TXT' in r, r)
        r = vm.run('cd /home/guest && ln F.TXT HARD && cat HARD')
        check('hard link shares content', r.strip() == 'hello', r)
        r = vm.run('cd /home/guest && stat HARD | grep Links')
        check('hard link bumps nlink', 'Links: 2' in r, r)
        r = vm.run('cat /latest')
        check('disk symlink /latest resolves to HELLO.TXT', r.startswith('Hello from StarlightOS'), r)
        r = vm.run('cd /home/guest && cat ~/F.TXT')
        check('~ expands inside a path', r.strip() == 'hello', r)

        # --- direct script execution + exit status ---
        vm.run("cd /home/guest && printf '#!/bin/sh\\necho ran from a script\\n' > S.SH && chmod 755 S.SH")
        r = vm.run('cd /home/guest && ./S.SH')
        check('shebang script runs directly', 'ran from a script' in r, r)
        r = vm.run('cd /home/guest && ./S.SH && echo rc=$?')
        check('script exit status is 0', 'rc=0' in r, r)
        r = vm.run('cd /home/guest && chmod 000 S.SH && ./S.SH; echo rc=$?')
        check('no exec permission -> 126', 'permission denied' in r and 'rc=126' in r, r)
        r = vm.run('bogusxyz; echo rc=$?')
        check('unknown command -> 127', 'command not found' in r and 'rc=127' in r, r)
        r = vm.run('cd /home/guest && chmod 755 S.SH && ./NOPE.SH; echo rc=$?')
        check('missing script -> 127', 'command not found' in r and 'rc=127' in r, r)
    finally:
        vm.quit()

    img = Ext2Image(vm.disk)
    def walk_ino(path):
        ino = 2
        for part in [p for p in path.split('/') if p]:
            hit = None
            for name, e, _ft in img._dir_entries(img._ino(ino)):
                if name == part:
                    hit = e
                    break
            if hit is None:
                return None
            ino = hit
        return ino
    def dirents(path):
        ino = walk_ino(path)
        return img._dir_entries(img._ino(ino)) if ino else []
    um = walk_ino('UM.TXT')
    check('disk: umask file persisted as 0600', bool(um) and (img._le16(img._ino(um), 0) & 0o777) == 0o600,
          oct(img._le16(img._ino(um), 0) & 0o777) if um else 'UM.TXT missing')
    ow = walk_ino('OW.TXT')
    check('disk: chown root persisted uid/gid 0', bool(ow) and img._le16(img._ino(ow), 2) == 0
          and img._le16(img._ino(ow), 24) == 0, 'uid/gid not 0' if ow else 'OW.TXT missing')
    f = img.read('home/guest/F.TXT')
    check('disk: symlink target file written through', f == b'hello\n', repr(f))
    t = img.read('tmp/tee.txt')
    check('disk: tee output persisted', t == b'abc\n', repr(t))
    lnk = [x for x in dirents('home/guest') if x[0] == 'LNK']
    check('disk: LNK entry is a symlink (ftype 7)', bool(lnk) and lnk[0][2] == 7, repr(lnk))
    lat = [x for x in dirents('') if x[0] == 'latest']
    check('disk: /latest entry is a symlink (ftype 7)', bool(lat) and lat[0][2] == 7, repr(lat))


def group_gui(work):
    print('\n[gui: ring-3 window syscalls (fbmap/present/getevents)]')
    vm = VM(DISK, work)
    try:
        if not check('boots to a shell prompt', vm.boot(), vm.log()[-200:]):
            return

        # (a) The app paints, presents, polls and exits on its own (no input).
        r = vm.run('run GDEMO.ELF', timeout=30)
        check('fbmap opens a window and maps its surface', 'GDEMO: fbmap ok' in r, r[-400:])
        check('present() before fbmap fails with EBADF',
              'present before fbmap fails' in r and 'errno=9' in r, r[-400:])
        check('the mapped surface is read/write', 'GDEMO: pixels readable' in r, r[-400:])
        check('present() composites the desktop', 'GDEMO: present ok' in r, r[-400:])
        check('re-fbmap at the same size is idempotent', 'GDEMO: fbmap again same' in r, r[-400:])
        check('fbmap resize returns EBUSY', 'GDEMO: fbmap resize busy' in r and 'errno=16' in r, r[-400:])
        check('getevents() poll times out with 0 events', 'GDEMO: poll n=0' in r, r[-400:])
        check('the app runs to completion', 'GDEMO: done' in r, r[-400:])
        r = vm.run('echo $?')
        check('exit status 0 after the window closes', r.strip() == '0', r)

        # (b) A key typed into the window reaches getevents() and quits the app.
        start = len(vm.log())
        vm.type('run GDEMO.ELF\n')
        check('the app announces it is waiting for input',
              vm.wait_for('GDEMO: waiting', 20, start), vm.log()[start:][-400:])
        vm.type('q')
        check('typed key is delivered as a SL_EV_KEY event',
              vm.wait_for('GDEMO: key 0x71', 10, start), vm.log()[start:][-400:])
        check('the app exits after the quit key',
              vm.wait_for('GDEMO: done', 10, start), vm.log()[start:][-400:])
        r = vm.run('echo $?')
        check('exit status 0 after a key-driven close', r.strip() == '0', r)
    finally:
        vm.quit()


def group_appnet(work):
    print('\n[appnet: ring-3 network syscalls (socket/connect/send/recv/poll/resolve)]')
    stop = threading.Event()
    http_srv = Threaded(('0.0.0.0', 18001), HTTP)
    threads = [threading.Thread(target=http_srv.serve_forever, daemon=True),
               threading.Thread(target=echo_server, args=(18002, stop), daemon=True)]
    for t in threads: t.start()
    vm = VM(DISK, work)
    try:
        if not check('boots to a shell prompt', vm.boot(), vm.log()[-200:]): return
        if not check('DHCP lease acquired', vm.wait_lease(), vm.log()[-300:]): return

        # NDEMO exercises the syscalls against the HTTP fixture (10.0.2.2:18001)
        # and an echo server (10.0.2.2:18002); every marker also reaches COM1.
        r = vm.run('run NDEMO.ELF', timeout=60)
        check('resolve() returns a literal IPv4 address',
              'NDEMO: resolve literal rc=0 ip=10.0.2.2' in r, r[-500:])
        check('socket() returns a usable fd',
              'NDEMO: socket fd=' in r and 'NDEMO: socket failed' not in r, r[-500:])
        check('connect() completes the TCP handshake', 'NDEMO: connect ok' in r, r[-500:])
        check('send() writes the request',
              'NDEMO: sent ' in r and 'NDEMO: sent -1' not in r, r[-500:])
        check('poll()+recv() read the HTTP status and body',
              '200 OK' in r and 'Hello, StarlightOS!' in r and 'line two' in r, r[-800:])
        check('recv() reports EOF when the server closes', 'NDEMO: http eof after' in r, r[-500:])
        check('a second connection reads the echo banner', 'NDEMO: banner WELCOME' in r, r[-500:])
        check('poll()+send()+recv() echo the payload', 'NDEMO: echo ECHO:ping-pong' in r, r[-500:])
        check('connect() to a closed port is refused', 'NDEMO: refused rc=-1' in r, r[-500:])
        check('socket() rejects an unsupported family', 'NDEMO: bad-domain fd=-1 errno=97' in r, r[-500:])
        check('send() on an unconnected socket is ENOTCONN',
              'NDEMO: unconnected send rc=-1 errno=107' in r, r[-500:])
        check('the app runs to completion', 'NDEMO: done' in r, r[-500:])
        r = vm.run('echo $?')
        check('exit status 0', r.strip() == '0', r)
    finally:
        stop.set()
        vm.quit()
        http_srv.shutdown()


def group_web(work):
    print('\n[web: libslweb userland URL + HTTP + HTML on the M2 sockets]')
    http_srv = Threaded(('0.0.0.0', 18001), HTTP)
    t = threading.Thread(target=http_srv.serve_forever, daemon=True)
    t.start()
    vm = VM(DISK, work)
    try:
        if not check('boots to a shell prompt', vm.boot(), vm.log()[-200:]): return
        if not check('DHCP lease acquired', vm.wait_lease(), vm.log()[-300:]): return

        # WEBTEST.ELF links libslweb and drives the same HTTP fixture as NDEMO
        # (10.0.2.2:18001: /hello /redir /loop /chunked /page).
        r = vm.run('run WEBTEST.ELF', timeout=60)
        check('libslweb: URL parses host/port/path (fragment dropped)',
              'WEBTEST: url host=10.0.2.2 port=18001 https=0 path=/hello?x=1' in r, r[-600:])
        check('libslweb: GET status + Content-Type + body',
              'WEBTEST: http err=0 status=200 type=text/plain len=29' in r
              and 'Hello, StarlightOS!' in r and 'line two' in r, r[-900:])
        check('libslweb: relative 302 followed to the final URL',
              'WEBTEST: redir err=0 status=200 redirects=1 final=http://10.0.2.2:18001/hello' in r, r[-700:])
        check('libslweb: redirect loop is cut off',
              'WEBTEST: loop err=-6' in r, r[-500:])
        check('libslweb: chunked body decoded',
              'WEBTEST: chunked err=0 status=200 len=5000' in r, r[-500:])
        check('libslweb: HTML rendered to text (links numbered, script skipped)',
              'Big Title' in r and 'bold' in r and '* one' in r and '[1]' in r
              and 'hidden' not in r and '<h1>' not in r, r[-900:])
        check('libslweb: https deferred to M9 with an unverified notice',
              'WEBTEST: https err=-12' in r and 'M9' in r, r[-500:])
        check('libslweb: connection refused mapped', 'WEBTEST: refused err=-2' in r, r[-500:])
        check('libslweb: bad URL rejected', 'WEBTEST: badurl err=-11' in r, r[-500:])
        check('the app runs to completion', 'WEBTEST: done' in r, r[-500:])
        r = vm.run('echo $?')
        check('exit status 0', r.strip() == '0', r)
    finally:
        vm.quit(); http_srv.shutdown()


def group_browse(work):
    print('\n[browse: M6 DOM + box-layout + CSS + images browser (M1 window + libslweb)]')
    http_srv = Threaded(('0.0.0.0', 18001), HTTP)
    t = threading.Thread(target=http_srv.serve_forever, daemon=True)
    t.start()
    vm = VM(DISK, work)
    try:
        if not check('boots to a shell prompt', vm.boot(), vm.log()[-200:]): return
        if not check('DHCP lease acquired', vm.wait_lease(), vm.log()[-300:]): return

        # The browser keeps its window open, so type the command and drive it with
        # keys instead of vm.run() (which waits for a shell prompt).
        start = len(vm.log())
        vm.type('run BROWSER.ELF http://10.0.2.2:18001/page\n')
        check('the browser opens a window', vm.wait_for('BROWSER: fbmap ok', 20, start), vm.log()[start:][-400:])
        check('fetches the page over the M2 sockets + libslweb',
              vm.wait_for('BROWSER: fetch status=200 type=text/html', 30, start), vm.log()[start:][-600:])
        seg = vm.log()[start:]
        check('DOM laid out into boxes',
              'BROWSER: layout boxes=' in seg and 'doc_h=' in seg, seg[-600:])
        check('laid-out text (heading, list, entity)',
              'Big Title' in seg and '* one' in seg and 'entity' in seg, seg[-900:])
        check('script/style/head content is skipped', 'hidden' not in seg and 'color:red' not in seg, seg[-900:])
        check('links are collected in document order',
              'links=1' in seg and 'BROWSER: link 1 href=/hello' in seg, seg[-900:])

        # Follow link 1 (/hello), resolved against the page URL.
        vm.key('1')
        check('following a link loads and renders the target',
              vm.wait_for('BROWSER: follow 1 -> http://10.0.2.2:18001/hello', 20, start)
              and vm.wait_for('BROWSER: fetch status=200 type=text/plain len=29', 20, start),
              vm.log()[start:][-600:])

        # Back to the page, then reload it.
        vm.key('b')
        check('back returns to the previous URL',
              vm.wait_for('BROWSER: back -> http://10.0.2.2:18001/page', 20, start), vm.log()[start:][-500:])
        vm.key('r')
        check('reload re-fetches the current URL',
              vm.wait_for('BROWSER: reload url=http://10.0.2.2:18001/page', 20, start), vm.log()[start:][-500:])

        vm.key('q')
        check('the browser exits on the quit key', vm.wait_for('BROWSER: done', 10, start), vm.log()[start:][-400:])
        r = vm.run('echo $?')
        check('exit status 0', r.strip() == '0', r)

        # An https:// URL is refused with the M9 "unverified connection" notice.
        start2 = len(vm.log())
        vm.type('run BROWSER.ELF https://example.com/\n')
        check('https page reports an unverified connection',
              vm.wait_for('BROWSER: https unverified', 20, start2), vm.log()[start2:][-400:])
        check('https fails with the M9 code and badge text',
              'BROWSER: error err=-12' in vm.log()[start2:] and 'unverified' in vm.log()[start2:],
              vm.log()[start2:][-500:])
        vm.key('q')
        check('the second run exits', vm.wait_for('BROWSER: done', 10, start2), vm.log()[start2:][-300:])
        r = vm.run('echo $?')
        check('exit status 0 after the https run', r.strip() == '0', r)

        # M6: the CSS subset + bitmap images, from a data: URI and an HTTP fetch.
        start3 = len(vm.log())
        vm.type('run BROWSER.ELF http://10.0.2.2:18001/m6page\n')
        check('the browser fetches the styled image page',
              vm.wait_for('BROWSER: fetch status=200 type=text/html', 30, start3), vm.log()[start3:][-600:])
        m6 = vm.log()[start3:]
        check('css: <style> rules compiled and applied to boxes',
              'BROWSER: css rules=3 images=2' in m6 and 'BROWSER: css applied fg=#3366ff' in m6, m6[-900:])
        check('images: inline data: URI decoded',
              'BROWSER: image 1 src=data:image/png;base64,' in m6 and 'w=2 h=2 alpha=1' in m6, m6[-900:])
        check('images: linked PNG fetched over HTTP',
              'BROWSER: image 2 src=/pic.png w=2 h=2 alpha=1' in m6, m6[-900:])
        check('styled text laid out (heading + 24px paragraph)',
              'Styled Heading' in m6 and 'Underlined big text' in m6, m6[-900:])
        vm.key('q')
        check('the styled run exits on the quit key', vm.wait_for('BROWSER: done', 10, start3), vm.log()[start3:][-400:])
        r = vm.run('echo $?')
        check('exit status 0 after the styled run', r.strip() == '0', r)

        # M7: chrome -- address bar, back/forward from the cache, bookmarks, forms.
        start4 = len(vm.log())
        vm.type('run BROWSER.ELF http://10.0.2.2:18001/page\n')
        check('the M7 browser opens', vm.wait_for('BROWSER: fetch status=200 type=text/html', 20, start4),
              vm.log()[start4:][-500:])

        # Address bar: Ctrl-L focuses it, typing + Enter navigates.
        vm.key('ctrl-l')
        check('ctrl-l focuses the address bar',
              vm.wait_for('BROWSER: addr focus', 10, start4), vm.log()[start4:][-300:])
        vm.type('http://10.0.2.2:18001/hello\n')
        check('typing a URL and pressing enter navigates',
              vm.wait_for('BROWSER: go http://10.0.2.2:18001/hello', 10, start4)
              and vm.wait_for('BROWSER: fetch status=200 type=text/plain len=29', 20, start4),
              vm.log()[start4:][-600:])

        # Back and forward are served from the M7 cache.
        vm.key('b')
        check('back is served from the cache',
              vm.wait_for('BROWSER: back -> http://10.0.2.2:18001/page', 20, start4)
              and 'BROWSER: cache hit url=http://10.0.2.2:18001/page' in vm.log()[start4:],
              vm.log()[start4:][-600:])
        vm.key('f')
        check('forward is served from the cache',
              vm.wait_for('BROWSER: forward -> http://10.0.2.2:18001/hello', 10, start4)
              and 'BROWSER: cache hit url=http://10.0.2.2:18001/hello' in vm.log()[start4:],
              vm.log()[start4:][-600:])

        # Bookmarks: Ctrl-D adds the current URL, Ctrl-B + 1 reopens it.
        vm.key('ctrl-d')
        check('ctrl-d bookmarks the current page',
              vm.wait_for('BROWSER: bookmark add url=http://10.0.2.2:18001/hello count=1', 10, start4),
              vm.log()[start4:][-400:])
        vm.key('ctrl-l'); vm.type('http://10.0.2.2:18001/page\n')
        vm.wait_for('BROWSER: fetch status=200 type=text/html', 20, start4)
        vm.key('ctrl-b')
        vm.key('1')
        check('a bookmark reopens its URL',
              vm.wait_for('BROWSER: bookmark open 1 -> http://10.0.2.2:18001/hello', 10, start4),
              vm.log()[start4:][-500:])

        # Forms: Tab to a field, type, Tab to the checkbox, space, Enter submits.
        vm.key('ctrl-l')
        vm.type('http://10.0.2.2:18001/form\n')
        check('the form page loads',
              vm.wait_for('BROWSER: load url=http://10.0.2.2:18001/form', 20, start4), vm.log()[start4:][-500:])
        vm.key('tab')
        check('tab focuses the first form field',
              vm.wait_for('BROWSER: field focus 1', 10, start4), vm.log()[start4:][-300:])
        vm.type('hi')
        vm.key('tab')
        vm.key('spc')
        vm.key('ret')
        check('editing a form and pressing enter submits it (GET)',
              vm.wait_for('BROWSER: submit method=get fields=2 url=http://10.0.2.2:18001/echo?q=hi&c=on', 15, start4),
              vm.log()[start4:][-700:])
        check('the submitted query reaches the server',
              vm.wait_for('echo:/echo?q=hi&c=on', 20, start4), vm.log()[start4:][-700:])

        vm.key('q')
        check('the M7 run exits on the quit key', vm.wait_for('BROWSER: done', 10, start4), vm.log()[start4:][-400:])
        r = vm.run('echo $?')
        check('exit status 0 after the M7 run', r.strip() == '0', r)

        # Bookmarks persist in BOOKMARKS.TXT across browser runs.
        start5 = len(vm.log())
        vm.type('run BROWSER.ELF http://10.0.2.2:18001/page\n')
        check('bookmarks are reloaded from BOOKMARKS.TXT on startup',
              vm.wait_for('BROWSER: bookmarks loaded count=1', 20, start5), vm.log()[start5:][-400:])
        vm.key('ctrl-b')
        vm.key('1')
        check('a persisted bookmark opens after a restart',
              vm.wait_for('BROWSER: bookmark open 1 -> http://10.0.2.2:18001/hello', 10, start5),
              vm.log()[start5:][-500:])
        vm.key('q')
        check('the persistence run exits on the quit key',
              vm.wait_for('BROWSER: done', 10, start5), vm.log()[start5:][-300:])
        r = vm.run('echo $?')
        check('exit status 0 after the persistence run', r.strip() == '0', r)
    finally:
        vm.quit(); http_srv.shutdown()
    bm = Ext2Image(vm.disk).read('BOOKMARKS.TXT')
    check('BOOKMARKS.TXT on disk holds the saved bookmark',
          bm is not None and b'http://10.0.2.2:18001/hello' in bm, repr(bm))


GROUPS = {'shell': group_shell, 'net': group_net, 'sound': group_sound, 'unix': group_unix, 'gui': group_gui, 'appnet': group_appnet, 'web': group_web, 'browse': group_browse}

def main():
    if not os.path.exists(DISK):
        print('disk.img not found: run `make disk.img` first'); return 2
    if subprocess.call(['which', 'qemu-system-x86_64'], stdout=subprocess.DEVNULL) != 0:
        print('qemu-system-x86_64 not found'); return 2
    wanted = sys.argv[1:] or list(GROUPS)
    for name, fn in GROUPS.items():
        if any(w in name for w in wanted):
            with tempfile.TemporaryDirectory(prefix='starlightos-test-') as d:
                fn(d)
    failed = [n for n, ok in results if not ok]
    print(f'\n{len(results) - len(failed)}/{len(results)} checks passed')
    for n in failed: print('  FAILED:', n)
    return 1 if failed else 0

if __name__ == '__main__':
    sys.exit(main())
