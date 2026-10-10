#!/usr/bin/env python3
"""Unit tests for telnet.c's protocol engine. usage: telnet_test.py /path/to/telnet_cli"""
import sys, subprocess
cli = sys.argv[1]
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
ECHO, SGA, TTYPE, NAWS, LINEMODE, NEWENV = 1, 3, 24, 31, 34, 39
def hx(b): return bytes(b).hex()
def run(stream, chunk=4096):
    r = subprocess.run([cli], input=f"CHUNK {chunk} {hx(stream)}\n", capture_output=True, text=True).stdout.split("\n")
    return r[0][4:], bytes.fromhex(r[1][5:]), int(r[2][5:])
def key(code): return subprocess.run([cli], input=f"KEY {code}\n", capture_output=True, text=True).stdout.strip()
fails = 0
def check(name, got, want):
    global fails
    if got != want: fails += 1; print(f"FAIL {name}\n   got  {got!r}\n   want {want!r}")
    else: print(f"ok   {name}")

# 1. classic server greeting: WILL ECHO, WILL SGA, DO TTYPE, DO NAWS, DO LINEMODE(unsupported), DO NEWENV
greet = bytes([IAC, WILL, ECHO, IAC, WILL, SGA, IAC, DO, TTYPE, IAC, DO, NAWS, IAC, DO, LINEMODE, IAC, WILL, NEWENV]) + b"login: "
want_sent = bytes([IAC, DO, ECHO, IAC, DO, SGA, IAC, WILL, TTYPE, IAC, WILL, NAWS,
                   IAC, SB, NAWS, 0, 80, 0, 32, IAC, SE,                       # window size follows WILL NAWS
                   IAC, WONT, LINEMODE, IAC, DONT, NEWENV])
for chunk in (4096, 1, 2, 3, 5):                                               # every split point, incl. mid-IAC
    out, sent, echo = run(greet, chunk)
    check(f"greeting, chunk={chunk}", (out, sent, echo), ("login: ", want_sent, 1))

# 2. TTYPE SEND subnegotiation answered with IS "DUMB"
out, sent, _ = run(bytes([IAC, SB, TTYPE, 1, IAC, SE]) + b"hi")
check("TTYPE SEND -> IS DUMB", (out, sent), ("hi", bytes([IAC, SB, TTYPE, 0]) + b"DUMB" + bytes([IAC, SE])))

# 3. no reply loops: repeating the same request twice only answers once; WONT for something never enabled is silent
out, sent, echo = run(bytes([IAC, WILL, ECHO, IAC, WILL, ECHO, IAC, WONT, SGA]))
check("idempotent negotiation", (sent, echo), (bytes([IAC, DO, ECHO]), 1))
out, sent, echo = run(bytes([IAC, WILL, ECHO, IAC, WONT, ECHO]))
check("WONT ECHO leaves char mode", (sent, echo), (bytes([IAC, DO, ECHO, IAC, DONT, ECHO]), 0))

# 4. text handling: CR LF, CR NUL, bare LF, tabs (to next multiple of 8), 0xFF escape, UTF-8, ANSI
out, _, _ = run(b"a\r\nb\r\0c\n")
check("CRLF / CR NUL / LF", out, "a\\x0d\\x0a" + "b\\x0d" + "c\\x0a")
out, _, _ = run(b"ab\tc")
check("tab expands to column 8", out, "ab      c")
out, _, _ = run(b"x" + bytes([IAC, IAC]) + b"y")
check("IAC IAC shown as ?", out, "x?y")
out, _, _ = run("caf\u00e9 \u2713 ok".encode())
check("UTF-8 collapsed to ?", out, "caf? ? ok")
out, _, _ = run(b"\x1b[1;31mred\x1b[0m \x1b(Bplain\x1bc.\x1b[2J!")
check("ANSI escapes stripped", out, "red plain.!")
out, _, _ = run(b"\x1b[1;3" ); out2, _, _ = run(b"\x1b[1;3")
check("truncated escape doesn't crash", out, "")

# 5. keys in character mode
check("Enter -> CR NUL", key(10), "0d00"); check("Backspace -> DEL", key(8), "7f")
check("Up arrow", key(0x81), "1b5b41"); check("Delete", key(0x87), "1b5b337e")
check("letter", key(ord('q')), "71"); check("Ctrl-D", key(4), "04"); check("F1 (nothing)", key(0x90), "")
print("ALL OK" if not fails else f"{fails} FAILED"); sys.exit(1 if fails else 0)
