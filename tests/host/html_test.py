#!/usr/bin/env python3
"""Regression + fuzz tests for html.c (the fetch HTML->text renderer). usage: html_test.py /path/to/html_cli"""
import sys, subprocess, random
cli = sys.argv[1]
def render(h): return subprocess.run([cli], input=h.encode(), capture_output=True, timeout=20)
fails = 0
def check(name, html, must=(), mustnot=()):
    global fails
    r = render(html); out = r.stdout.decode(errors="replace")
    ok = r.returncode == 0 and all(m in out for m in must) and not any(m in out for m in mustnot)
    if not ok: fails += 1
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else f"\n   rc={r.returncode} out={out[:200]!r} err={r.stderr.decode()[:300]}"))

check("simple", "<html><head><title>T</title></head><body><h1>Hi</h1><p>there</p></body></html>", ["# Hi", "there"], ["T\n"])
check("head never closed", "<html><head><title>x</title><body><p>visible</p>", ["visible"])
check("no head, title only", "<title>x</title><p>visible</p>", ["visible"])
check("uppercase old-school", "<HTML><HEAD><TITLE>x<BODY><H1>Old</H1>", ["# Old"])
check("js with < and quotes", "<script>for(i=0;i<n;i++){s+='<b>'};if(a<'b){x()}var q=\"it's\"</script><p>after</p>", ["after"], ["x()"])
check("document.write of script tag", "<script>document.write('<script src=a.js><\\/script>')</script><p>after</p>", ["after"])
check("fake </script> inside string split", "<script>var s='</scr'+'ipt>';</script><p>after</p>", ["after"])
check("ld+json", "<script type=application/ld+json>{\"a\":\"<b>\"}</script><p>after</p>", ["after"], ["{"])
check("inline svg + noscript + style", "<style>a>b{}</style><svg><g><path d='M0'/></g></svg><noscript><p>enable js</p></noscript><p>after</p>", ["after"], ["enable js", "M0"])
check("stray < in text", "<p>1 < 2 and 3 > 2</p><p>after</p>", ["after"])
check("unterminated tag at EOF", "<p>before</p><a href='x", ["before"])
check("lone quote in text", "<p>don't stop</p><p>after</p>", ["after", "don't"])
check("self-closing script is not raw", "<script src=x.js /><p>after</p>", ["after"])
check("links numbered", "<a href='https://a.example/x'>A</a> <a href='#top'>top</a>", ["A[1]", "[1] https://a.example/x"], ["#top"])
big = "<html><head><title>g</title>" + "<script nonce=1>" + "var a=\"<div>\";if(b<'x'){}" * 4000 + "</script></head><body><h1>Big</h1>" + "<p>word</p>" * 500 + "</body></html>"
check("100 KB script then body", big, ["# Big", "word"])
random.seed(7)
frags = ["<p>", "</p>", "<div>", "<script>", "</script>", "<style>", "</style>", "<a href='x'>", "</a>", "<", ">", "'", '"', "=", "&amp;", "&#x41;", "&", "<!--", "-->", "<head>", "</head>", "<svg>", "</svg>", "<ul><li>", "<ol><li>", "</li>", "<pre>", "</pre>", "text ", " ", "\n", "<img alt='z'>", "<h3>", "<br/>", "<td>", "<tr>", "<hr>", "<![CDATA[", "\x00", "\xff", "<noscript>", "</noscript>", "<title>", "</title>"]
for n in range(300):
    h = "".join(random.choice(frags) for _ in range(random.randint(1, 120)))
    r = subprocess.run([cli], input=h.encode("latin-1"), capture_output=True, timeout=20)
    if r.returncode != 0:
        fails += 1; print("FUZZ CRASH", repr(h)[:200], r.stderr.decode()[:300]); break
else: print("ok   300 fuzz inputs, no crash / sanitizer reports")
print("ALL OK" if not fails else f"{fails} FAILED"); sys.exit(1 if fails else 0)
