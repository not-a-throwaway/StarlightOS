#!/usr/bin/env python3
"""Unit tests for libslweb (url.c + http.c + dom.c + layout.c) using the
scripted-socket host driver.

    make slwebtest                     # or: python3 tests/host/slweb_test.py /tmp/slweb_host

The HTML->text renderer is covered by html_test.py, which the Makefile runs
against the same binary (its no-arg mode is stdin HTML -> stdout text). The M5
DOM and box-layout passes have their own `dom` and `layout WIDTH` modes here.
"""
import base64
import struct
import subprocess
import sys
import zlib

CLI = sys.argv[1]
fails = 0


def check(name, cond, detail=''):
    global fails
    if not cond:
        fails += 1
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else f"   <- {detail!r}"))


def run(args, script=b''):
    r = subprocess.run([CLI] + args, input=script, capture_output=True, timeout=20)
    if r.returncode != 0:
        return None, r.stderr.decode(errors='replace')
    return r.stdout.decode(errors='replace'), ''


def fields(out):
    d = {}
    for line in out.splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            d[k] = v
    return d


# ------------------------------------------------------------------ URL parsing
def url(u, **want):
    out, err = run(['url', u])
    if out is None:
        check('url ' + u, False, err); return
    d = fields(out)
    ok = all(d.get(k) == str(v) for k, v in want.items())
    check('url ' + u, ok, d)


url('http://example.com/path?q=1#frag', rc=0, host='example.com', port='80', https='0', path='/path?q=1')
url('https://a.test:8443/x', rc=0, host='a.test', port='8443', https='1', path='/x')
url('example.com', rc=0, host='example.com', port='80', https='0', path='/')
url('http://example.com', rc=0, path='/')
url('http://example.com:0/', rc=-1)
url('http://[::1]/', rc=-1)          # IPv6 literal rejected
url('http:///nohost', rc=-1)         # empty host
url('http://host:99999/', rc=-1)     # port out of range

# ------------------------------------------------------------------ HTTP parsing
def http(name, url_, script, hops=5, req_method=None, req_body=None, contains=None, **want):
    args = ['http', url_, str(hops)]
    if req_method is not None:
        args.append(req_method)
        if req_body is not None:
            args.append(req_body)
    out, err = run(args, script.encode('latin-1') if isinstance(script, str) else script)
    if out is None:
        check(name, False, err); return
    d = fields(out)
    ok = all(d.get(k) == str(v) for k, v in want.items())
    if ok and contains:
        ok = all(d.get(k, '').find(sub) >= 0 for k, sub in contains.items())
    check(name, ok, d)


OK = b'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello'

http('content-length body', 'http://10.0.2.2:18001/x', OK,
     error=0, status=200, status_text='OK', content_type='text/plain', content_length=5, body_len=5, body='hello')
http('request line + Host header', 'http://10.0.2.2:18001/x', OK,
     contains={'request': 'GET /x HTTP/1.1\\r\\nHost: 10.0.2.2:18001\\r\\n'})

http('https rejected with a clear code', 'https://example.com/', b'',
     error=-12, error_text='https:// needs the userland TLS client (M9)')

http('no response times out', 'http://10.0.2.2:18001/x', b'',
     error=-4, error_text='no response (timed out)')

http('unresolvable host', 'http://bad.invalid/x', b'',
     error=-1, error_text='could not resolve host')

http('connection refused (port 1)', 'http://10.0.2.2:1/x', b'',
     error=-2, error_text='connection failed')

http('malformed status line', 'http://10.0.2.2:18001/x', b'NOTHTTP 200 OK\r\n\r\n',
     error=-5, error_text='malformed HTTP response')

# read-until-close (no Content-Length, no chunked)
http('read-until-close', 'http://10.0.2.2:18001/x',
     b'HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\nbody till eof',
     error=0, status=200, content_length=-1, body_len=13, body='body till eof')

# chunked transfer
CHUNKED = (b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n'
           b'5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n')
http('chunked decoding', 'http://10.0.2.2:18001/x', CHUNKED,
     error=0, status=200, content_length=-1, body_len=11, body='hello world')

# HEAD: headers only, no body (even with a Content-Length)
http('HEAD has no body', 'http://10.0.2.2:18001/head',
     b'HTTP/1.1 200 OK\r\nContent-Length: 999\r\nX-Test: yes\r\n\r\n', req_method='HEAD',
     error=0, status=200, content_length=999, body_len=0,
     contains={'request': 'HEAD /head HTTP/1.1'})

# POST: body appended and Content-Length sent
http('POST body + Content-Length', 'http://10.0.2.2:18001/post',
     b'HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nok!', req_method='POST', req_body='a=1',
     error=0, status=200, body='ok!', body_len=3,
     contains={'request': 'Content-Length: 3\\r\\n\\r\\na=1'})

# 404 keeps its status and body
http('404 status and body', 'http://10.0.2.2:18001/missing',
     b'HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nContent-Length: 4\r\n\r\nnope',
     error=0, status=404, status_text='Not Found', body='nope')

# ------------------------------------------------------------------ redirects
R302 = 'HTTP/1.1 302 Found\r\nLocation: http://10.0.2.2:18001/hello\r\nContent-Length: 0\r\nConnection: close\r\n\r\n'
R200 = 'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nhi'
http('follows 302 to 200', 'http://10.0.2.2:18001/redir', R302 + '@@@' + R200,
     error=0, status=200, redirects=1, final_url='http://10.0.2.2:18001/hello', body='hi')

http('absolute redirect target', 'http://10.0.2.2:18001/a', 
     'HTTP/1.1 301 Moved\r\nLocation: http://other.test/z\r\nContent-Length: 0\r\n\r\n' + '@@@' +
     'HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabs',
     error=0, status=200, final_url='http://other.test/z', redirects=1, body='abs')

http('relative redirect resolved against the path', 'http://10.0.2.2:18001/a/b/page',
     'HTTP/1.1 302 Found\r\nLocation: child\r\nContent-Length: 0\r\n\r\n' + '@@@' + R200,
     error=0, final_url='http://10.0.2.2:18001/a/b/child', redirects=1)

http('redirect loop is cut off', 'http://10.0.2.2:18001/loop',
     R302.replace('/hello', '/loop') + '@@@' + R302.replace('/hello', '/loop') + '@@@' + R302.replace('/hello', '/loop'),
     hops=2, error=-6, error_text='too many redirects')

http('--no-redirect keeps the 302', 'http://10.0.2.2:18001/redir', R302, hops=0,
     error=0, status=302, redirects=0, location='http://10.0.2.2:18001/hello', body_len=0)

# ------------------------------------------------------------------ limits
http('body is truncated at max_body', 'http://10.0.2.2:18001/x',
     b'HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n' + bytes(range(100)),
     error=0, status=200, body_len=100)

# ------------------------------------------------------------------ URL resolution
def resolve(base, loc, **want):
    out, err = run(['resolve', base, loc])
    if out is None:
        check('resolve ' + loc, False, err); return
    d = fields(out)
    ok = all(d.get(k) == str(v) for k, v in want.items())
    check(f'resolve {loc!r} against {base!r}', ok, d)


resolve('http://10.0.2.2:18001/a/b/page', '/hello', rc=0, url='http://10.0.2.2:18001/hello')
resolve('http://10.0.2.2:18001/a/b/page', 'child', rc=0, url='http://10.0.2.2:18001/a/b/child')
resolve('http://10.0.2.2:18001/a/b/', 'child.html', rc=0, url='http://10.0.2.2:18001/a/b/child.html')
resolve('http://10.0.2.2:18001/', 'top', rc=0, url='http://10.0.2.2:18001/top')
resolve('http://host:8080/x/y', 'z', rc=0, url='http://host:8080/x/z')
resolve('http://host/x/y?q=1', 'z', rc=0, url='http://host/x/z')
resolve('https://secure.test/a/b', 'c', rc=0, url='https://secure.test/a/c')     # scheme + default port hidden
resolve('http://host/a', '//cdn.test/lib.js', rc=0, url='http://cdn.test/lib.js')
resolve('https://host/a', '//cdn.test/lib.js', rc=0, url='https://cdn.test/lib.js')
resolve('http://host/a', 'http://other.test/z', rc=0, url='http://other.test/z')
resolve('http://host/a', 'https://other.test/z', rc=0, url='https://other.test/z')
resolve('http://[::1]/x', '/x', rc=-1)

# ------------------------------------------------------------------ DOM (M5)
def dom_of(html):
    return run(['dom'], html.encode())


def dom_check(name, html, contains=(), absent=()):
    out, err = dom_of(html)
    if out is None:
        check(name, False, err); return
    ok = all(s in out for s in contains) and all(s not in out for s in absent)
    check(name, ok, out)


dom_check('dom basic tree + entity', '<p>a &amp; b</p>',
          contains=['#document', '.<p>', '..text=a & b'])
dom_check('dom raw elements keep no text',
          '<script>var x=1</script><title>T</title><p>hi</p>',
          contains=['<script>', '<title>', 'text=hi'],
          absent=['var x', 'text=T'])
dom_check('dom keeps <style> text for the CSS pass',
          '<style>p{color:red}</style><p>hi</p>',
          contains=['<style>', 'text=p{color:red}'], absent=[])
dom_check('dom keeps <textarea> text as its initial value',
          '<textarea name="t">hello</textarea>',
          contains=['<textarea', 'text=hello'], absent=[])
dom_check('dom attributes are lowercased', '<a class="x" href=/a target=_blank>go</a>',
          contains=['<a class=x href=/a target=_blank>'])
dom_check('dom comments + doctype skipped', '<!doctype html><!-- c --><p>x</p>',
          contains=['<p>', 'text=x'], absent=['doctype', '<!--'])
dom_check('dom void element does not nest', '<p>one<br>two</p>',
          contains=['<br>', 'text=two'])

_oli, _ = dom_of('<ul><li>a<li>b</ul>')
check('dom implicit </li>', _oli is not None and _oli.count('<li>') == 2, _oli)
_oem, _ = dom_of('<p>open<p>second')
check('dom implicit </p>', _oem is not None and _oem.count('<p>') == 2, _oem)
_orph, _ = dom_of('<p>x</div>y</p>')
check('dom orphan close tag ignored', _orph is not None and 'text=x' in _orph and 'text=y' in _orph, _orph)


# ------------------------------------------------------------------ box layout (M5)
def parse_layout(out):
    head, boxes, links = {}, [], []
    for line in out.splitlines():
        if line.startswith('box'):
            text = ''
            if ' text=' in line:
                line, _, text = line.partition(' text=')
            parts = line.split()
            d = {'idx': int(parts[0][3:]), 'text': text}
            for p in parts[1:]:
                k, v = p.split('=', 1)
                d[k] = int(v) if v.lstrip('-').isdigit() else v
            boxes.append(d)
        elif '=' not in line:
            continue
        else:
            k, v = line.split('=', 1)
            if k == 'n_links':
                head[k] = int(v)
            elif k.startswith('link'):
                links.append(v)
            else:
                head[k] = int(v) if v.lstrip('-').isdigit() else v
    return head, boxes, links


def layout(html, width):
    out, err = run(['layout', str(width)], html.encode())
    if out is None:
        return None, None, None, err
    h, b, l = parse_layout(out)
    return h, b, l, ''


def lay_check(name, html, width, cond):
    h, b, l, err = layout(html, width)
    ok = h is not None and cond(h, b, l)
    check(name, ok, (h, b, l, err))


lay_check('layout h1 is scale 2', '<h1>Hi</h1>', 200,
          lambda h, b, l: h['height'] == 32 and b[0]['kind'] == 0 and b[0]['scale'] == 2
          and b[0]['text'] == 'Hi' and b[0]['h'] == 16)
lay_check('layout paragraph wraps at the viewport', '<p>' + 'word ' * 40 + '</p>', 120,
          lambda h, b, l: len(b) > 1 and all(x['x'] + x['w'] <= 120 for x in b))
lay_check('layout collects links in order', '<p><a href="/a">A</a> <a href="/b">B</a></p>', 200,
          lambda h, b, l: h['n_links'] == 2 and l == ['/a', '/b']
          and any(x.get('link') == 1 for x in b) and any(x.get('link') == 2 for x in b))
lay_check('layout ul bullets with hanging indent', '<ul><li>one</li><li>two</li></ul>', 200,
          lambda h, b, l: len([x for x in b if x['kind'] == 3 and x['marker'] == 0]) == 2
          and all(x['x'] >= 20 for x in b if x['text'] in ('one', 'two')))
lay_check('layout ol numbers', '<ol start="3"><li>a</li><li>b</li></ol>', 200,
          lambda h, b, l: [x['marker'] for x in b if x['kind'] == 3] == [3, 4])
lay_check('layout hr spans the content', '<p>a</p><hr><p>b</p>', 200,
          lambda h, b, l: any(x['kind'] == 2 and x['w'] == 200 for x in b))
lay_check('layout img alt placeholder', '<p>a <img alt="pic"> b</p>', 200,
          lambda h, b, l: any(x['kind'] == 1 and x['text'] == 'pic' and x['w'] == 3 * 8 + 8 for x in b))
lay_check('layout blockquote indents', '<blockquote>q</blockquote>', 200,
          lambda h, b, l: any(x['text'] == 'q' and x['x'] == 20 for x in b))
lay_check('layout pre keeps spaces and newlines', '<pre>a  b\nc</pre>', 200,
          lambda h, b, l: any(x['text'] == 'a  b' for x in b) and any(x['text'] == 'c' for x in b))
lay_check('layout bold/italic flags', '<p><b>x</b> <i>y</i></p>', 200,
          lambda h, b, l: any(x['text'] == 'x' and x['bold'] == 1 for x in b)
          and any(x['text'] == 'y' and x['italic'] == 1 for x in b))
lay_check('layout script/style emit no boxes', '<style>a{}</style><script>z</script><p>ok</p>', 200,
          lambda h, b, l: any(x['text'] == 'ok' for x in b)
          and all(x['text'] not in ('a{}', 'z') for x in b))
lay_check('layout decodes entities', '<p>x &amp; y</p>', 200,
          lambda h, b, l: any(x['text'] == '&' for x in b))


# ------------------------------------------------------------------ link collection
def links(html, n, hrefs, text_has=()):
    out, err = run(['links'], html.encode())
    if out is None:
        check('links', False, err); return
    d = fields(out)
    ok = d.get('n_links') == str(n) and all(d.get(f'link{i}') == h for i, h in enumerate(hrefs))
    if ok and text_has:
        ok = all(s in d.get('text', '') for s in text_has)
    check('links ' + (html[:40]), ok, d)


links('<p><a href="/a">A</a> and <a href="/b">B</a></p>', 2, ['/a', '/b'],
      text_has=['[1]', '[2]', 'A', 'B'])
links('<a href="#top">skip</a> <a href="javascript:void(0)">skip</a> <a href="/real">keep</a>', 1, ['/real'])
links('<a>no href</a><a href="">empty</a><a href="/ok">ok</a>', 1, ['/ok'])
# the cap is SLWEB_MAX_LINKS (32)
links(''.join(f'<a href="/l{i}">n</a>' for i in range(40)), 32,
      [f'/l{i}' for i in range(32)])
# attributes may be quoted with single quotes and there may be other attrs first
links('<a class=x href="/one" target=_blank>go</a>', 1, ['/one'])


# ------------------------------------------------------------------ CSS (M6)
def css_of(html):
    return run(['css'], html.encode())


def css_check(name, html, contains=(), absent=()):
    out, err = css_of(html)
    if out is None:
        check(name, False, err); return
    ok = all(s in out for s in contains) and all(s not in out for s in absent)
    check(name, ok, out)


css_check('css tag selector + colour', '<style>p{color:#00ff00}</style><p>hi</p><div>no</div>',
          contains=['rules=1', 'el=p color=00ff00'], absent=['el=div'])
css_check('css id beats class beats tag', '<style>p{color:red}.c{color:green}#i{color:blue}</style>'
          '<p id=i class=c>z</p>',
          contains=['el=p#i.c color=0000ff'])
css_check('css inline style wins', '<style>p{color:red}</style><p style="color:#112233">z</p>',
          contains=['color=112233'])
css_check('css font-weight + font-size', '<style>p{font-weight:bold;font-size:24px}</style><p>z</p>',
          contains=['bold=1', 'scale=2'])
css_check('css margin + padding sides', '<style>p{margin:1px 2px 3px 4px;padding:5px}</style><p>z</p>',
          contains=['margin=1,2,3,4', 'pad=5,5,5,5'])
css_check('css text-align + display', '<style>p{text-align:right;display:none}</style><p>z</p>',
          contains=['align=2', 'display=0'])
css_check('css named colour and inherited rules count',
          '<style>h1{color:navy}p{color:#abc}</style><h1>a</h1><p>b</p>',
          contains=['rules=2', 'color=000080', 'color=aabbcc'])
css_check('css descendant selector', '<style>div p{color:red}</style><div><p>z</p></div><p>no</p>',
          contains=['el=p color=ff0000'])


# ------------------------------------------------------------------ images (M6)
def _png_chunk(typ, data):
    return (struct.pack('>I', len(data)) + typ + data +
            struct.pack('>I', zlib.crc32(typ + data) & 0xffffffff))


def make_png(w, h, rows_rgba):
    return make_png_ex(w, h, 8, 6, None, rows_rgba)


def make_png_ex(w, h, depth, ctype, plte, rows):
    out = (b'\x89PNG\r\n\x1a\n' +
           _png_chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, depth, ctype, 0, 0, 0)))
    if plte is not None:
        out += _png_chunk(b'PLTE', plte)
    raw = b''.join(b'\x00' + bytes(r) for r in rows)
    return out + _png_chunk(b'IDAT', zlib.compress(raw)) + _png_chunk(b'IEND', b'')


def make_bs64(w, h, pix, alpha=False):
    return (b'BS64' + bytes([1, 1 if alpha else 0, 0, 0]) +
            struct.pack('<HH', w, h) + bytes(pix))


def image_of(data):
    return run(['image'], data)


def img_check(name, data, **want):
    out, err = image_of(data)
    if out is None:
        check(name, False, err); return
    d = fields(out)
    ok = all(d.get(k) == str(v) for k, v in want.items())
    check(name, ok, d)


img_check('image decode PNG RGBA', make_png(2, 2, [[255, 0, 0, 255, 0, 255, 0, 255],
          [0, 0, 255, 255, 255, 255, 255, 128]]),
          format='png', rc=0, w=2, h=2, has_alpha=1, sum=1530, p0='ffff0000', plast='80ffffff')
img_check('image 8-bit greyscale PNG', make_png_ex(1, 1, 8, 0, None, [[0x80]]),
          format='png', rc=0, w=1, h=1, has_alpha=0, sum=384, p0='ff808080')
img_check('image palette PNG', make_png_ex(2, 1, 8, 3, bytes([0x0a, 0x14, 0x1e, 0x28, 0x32, 0x3c]), [[0, 1]]),
          format='png', rc=0, w=2, h=1, has_alpha=0, sum=210, p0='ff0a141e', plast='ff28323c')
img_check('image 1-bit greyscale PNG', make_png_ex(8, 1, 1, 0, None, [[0xf0]]),
          format='png', rc=0, w=8, h=1, has_alpha=0, sum=3060, p0='ffffffff', plast='ff000000')
img_check('image decode BS64 RGB', make_bs64(1, 2, [10, 20, 30, 40, 50, 60]),
          format='bs64', rc=0, w=1, h=2, has_alpha=0, sum=210, p0='ff0a141e', plast='ff28323c')
img_check('image BS64 with alpha', make_bs64(1, 1, [1, 2, 3, 4], alpha=True),
          format='bs64', rc=0, w=1, h=1, has_alpha=1, sum=6, p0='04010203')
img_check('image rejects junk', b'not an image at all', format='-', rc=-1)

# data: URI decoding, via layout
_DATA = base64.b64encode(make_png(2, 2, [[255, 0, 0, 255, 0, 255, 0, 255],
                                         [0, 0, 255, 255, 255, 255, 255, 128]])).decode()
lay_check('layout decodes an inline data: image',
          '<p><img src="data:image/png;base64,%s"></p>' % _DATA, 200,
          lambda h, b, l: h['images'] == 1 and any(x['kind'] == 1 and x.get('img') == '2x2' for x in b))


# ------------------------------------------------------------------ CSS applied by layout (M6)
lay_check('layout css colour to box fg', '<style>p{color:#0000ff}</style><p>hi</p>', 200,
          lambda h, b, l: any(x['text'] == 'hi' and x.get('fg') == '0000ff' for x in b))
lay_check('layout css margin offsets the box', '<style>p{margin:4px}</style><p>hi</p>', 200,
          lambda h, b, l: any(x['text'] == 'hi' and x['x'] == 4 and x['y'] == 4 for x in b))
lay_check('layout css background emits a RECT', '<style>p{background:#00ff00}</style><p>hi</p>', 200,
          lambda h, b, l: any(x['kind'] == 4 and x.get('bg') == '00ff00' for x in b)
          and any(x['text'] == 'hi' and x.get('bg') == '00ff00' for x in b))
lay_check('layout css font-size 24px -> scale 2', '<style>p{font-size:24px}</style><p>hi</p>', 200,
          lambda h, b, l: any(x['text'] == 'hi' and x['scale'] == 2 for x in b))
lay_check('layout css text-align center', '<style>p{text-align:center}</style><p>hi</p>', 200,
          lambda h, b, l: any(x['text'] == 'hi' and x['x'] > 0 for x in b))
lay_check('layout css underline flag', '<style>p{text-decoration:underline}</style><p>hi</p>', 200,
          lambda h, b, l: any(x['text'] == 'hi' and x.get('u') == 1 for x in b))
lay_check('layout css display none hides content', '<style>p{display:none}</style><p>gone</p>', 200,
          lambda h, b, l: all(x['text'] != 'gone' for x in b))

# ------------------------------------------------------------------ form controls (M7)
lay_check('layout text input is an INPUT box', '<form><input name=q></form>', 200,
          lambda h, b, l: len(b) == 1 and b[0]['kind'] == 5 and b[0]['w'] == 20 * 8 + 4
          and b[0]['el'] == 'input' and b[0]['name'] == 'q')
lay_check('layout input size= shrinks the box', '<form><input name=q size=5></form>', 200,
          lambda h, b, l: b[0]['w'] == 5 * 8 + 4)
lay_check('layout submit input is a BUTTON with its label', '<form><input type=submit value=Go></form>', 200,
          lambda h, b, l: b[0]['kind'] == 6 and b[0]['text'] == 'Go' and b[0]['w'] == 2 * 8 + 12)
lay_check('layout checkbox is a small INPUT box', '<form><input type=checkbox name=c></form>', 200,
          lambda h, b, l: b[0]['kind'] == 5 and b[0]['w'] == 12 and b[0]['h'] == 12
          and b[0]['type'] == 'checkbox')
lay_check('layout hidden input emits no box', '<form><input type=hidden name=h><input name=v></form>', 200,
          lambda h, b, l: len(b) == 1 and b[0].get('name') == 'v')
lay_check('layout textarea is cols x rows', '<form><textarea cols=10 rows=2></textarea></form>', 200,
          lambda h, b, l: b[0]['kind'] == 5 and b[0]['w'] == 10 * 8 + 4 and b[0]['h'] == 2 * 10 + 4)
lay_check('layout select is sized to its widest option',
          '<form><select name=s><option>hello</option><option>hi</option></select></form>', 200,
          lambda h, b, l: b[0]['kind'] == 5 and b[0]['w'] == 5 * 8 + 20)
lay_check('layout button label comes from its text', '<form><button>Press me</button></form>', 200,
          lambda h, b, l: b[0]['kind'] == 6 and b[0]['text'] == 'Press me' and b[0]['w'] == 8 * 8 + 12)

# ------------------------------------------------------------------ cache (M7)
def cache_script(lines):
    return ('\n'.join(lines) + '\n').encode()


_out, _err = run(['cache'], cache_script([
    'put http://a/ 200 text/plain hello',
    'put http://b/ 200 text/html bb',
    'put http://c/ 200 text/plain ccc',
    'get http://a/',
    'get http://b/',
    'put http://d/ 200 text/plain dddd',
    'get http://c/',
    'get http://d/',
]))
_cache = _out or ''
check('cache stores and replays a body',
      'get url=http://a/ hit status=200 type=text/plain len=5 body=hello' in _cache, _cache)
check('cache evicts the least-recently-used entry',
      'get url=http://c/ miss' in _cache, _cache)
check('cache keeps a recently used entry', 'get url=http://d/ hit' in _cache, _cache)
check('cache hit/miss counters', 'hits=3 misses=1' in _cache, _cache)

_out, _ = run(['cache'], cache_script([
    'put http://big/ 200 text/plain ' + 'z' * 41,       # over the 40-byte budget
    'get http://big/',
    'clear',
    'get http://big/',
]))
_cache = _out or ''
check('cache refuses a body larger than the budget', 'put rc=-1' in _cache, _cache)
check('cache clear drops entries', 'clear count=0' in _cache, _cache)

# ------------------------------------------------------------------ forms (M7)
def form_of(html, page_url):
    return run(['form', page_url], html.encode())


def form_check(name, html, page_url, contains=(), absent=()):
    out, err = form_of(html, page_url)
    if out is None:
        check(name, False, err); return
    ok = all(s in out for s in contains) and all(s not in out for s in absent)
    check(name, ok, out)


form_check('form collects text/checkbox/select/textarea (GET)',
           '<form action="/search" method="get">'
           '<input name="q" value="a b">'
           '<input type="checkbox" name="c1" checked>'
           '<input type="checkbox" name="c2">'
           '<select name="s"><option value="v1">One</option>'
           '<option value="v2" selected>Two</option></select>'
           '<textarea name="t">line</textarea>'
           '<input type="submit" name="go" value="Go"></form>',
           'http://example.com/search',
           contains=['n=4', 'pair0 name=q value=a b', 'pair1 name=c1 value=on',
                     'pair2 name=s value=v2', 'pair3 name=t value=line',
                     'query rc=23 val=q=a+b&c1=on&s=v2&t=line',
                     'submit rc=0 method=get',
                     'url=http://example.com/search?q=a+b&c1=on&s=v2&t=line'],
           absent=['name=go', 'name=c2'])

form_check('form POST encodes and keeps the action URL',
           '<form action="/submit" method="POST">'
           '<input name="user" value="a b&c">'
           '<input type="checkbox" name="x" checked></form>',
           'http://example.com/page',
           contains=['submit rc=0 method=post', 'url=http://example.com/submit',
                     'body=user=a+b%26c&x=on'])

form_check('form GET replaces the action query string',
           '<form action="/s?old=1"><input name="a" value="2"></form>',
           'http://example.com/',
           contains=['url=http://example.com/s?a=2'], absent=['old=1'])

form_check('form with a bad action URL fails cleanly',
           '<form action="http://[::1]/x"><input name="a" value="1"></form>',
           'http://example.com/', contains=['submit rc=-1'])

print('ALL OK' if not fails else f'{fails} FAILED')
sys.exit(1 if fails else 0)
