#!/usr/bin/env python3
"""Host tests for the StarlightOS shell: runs the real shell/VFS/ext2/devfs/procfs code
(tests/host/shell_host.c) against a copy of disk.img and checks each command's output + status.

usage: shell_test.py ./shell_host path/to/disk.img
"""
import shutil, subprocess, sys, tempfile, os

host, img = sys.argv[1], sys.argv[2]
tmp = tempfile.mkdtemp()
work = os.path.join(tmp, "disk.img")
shutil.copy(img, work)

# (command line, expected stdout, expected status or None)
T = [
 ("echo hi && echo yes || echo no", "hi\nyes\n", 0),
 ("false || echo fallback", "fallback\n", 0),
 ("false && echo no; echo after", "after\n", 0),
 ("true || echo no", "", 0),
 ("! false", "", 0),
 ("x=$(echo hello world); echo \"[$x]\"", "[hello world]\n", 0),
 ("echo $(echo one two) end", "one two end\n", 0),
 ("echo `echo bt`", "bt\n", 0),
 ("echo '$(echo no)'", "$(echo no)\n", 0),
 ("echo \"$(echo a; echo b)\"", "a\nb\n", 0),
 ("echo $(echo $(echo nested))", "nested\n", 0),
 ("echo a b | wc -w", "2 \n", 0),
 ("echo $(echo a b | wc -w)", "2\n", 0),
 ("echo *.TXT", "HELLO.TXT KERNEL.TXT README.TXT\n", 0),
 ("ls /etc/p*", "/etc/passwd\n/etc/profile\n", 0),
 ("echo /etc/p?ofile", "/etc/profile\n", 0),
 ("echo /etc/[gh]*", "/etc/group /etc/hostname\n", 0),
 ("echo nomatch*here", "nomatch*here\n", 0),
 ("echo '*'", "*\n", 0),
 ("cat /nonexist | wc -l", "cat: /nonexist: no such file\n0 \n", 0),
 ("cat /nonexist 2>/dev/null | wc -l", "0 \n", 0),
 ("cat /nonexist 2>&1 | wc -l", "1 \n", 0),
 ("cat /nonexist 2>/tmp/e.txt; cat /tmp/e.txt", "cat: /nonexist: no such file\n", 0),
 ("echo oops >&2 | wc -l", "oops\n0 \n", 0),
 ("echo to-file > /tmp/o.txt; echo more >> /tmp/o.txt; cat /tmp/o.txt", "to-file\nmore\n", 0),
 ("cat /nonexist &> /tmp/both.txt; cat /tmp/both.txt", "cat: /nonexist: no such file\n", 0),
 ("for i in 1 2 3; do echo n$i; done", "n1\nn2\nn3\n", 0),
 ("i=0; while test $i -lt 3; do echo w$i; i=$((i+1)); done", "w0\nw1\nw2\n", 1),
 ("i=3; until test $i -eq 0; do echo u$i; i=$((i-1)); done", "u3\nu2\nu1\n", 0),
 ("if test -f /etc/passwd; then echo found; else echo missing; fi", "found\n", 0),
 ("if false; then echo A; elif true; then echo B; else echo C; fi", "B\n", 0),
 ("if false; then echo A; else echo C; fi", "C\n", 0),
 ("for i in 1 2 3 4; do if test $i -eq 2; then continue; fi; if test $i -eq 4; then break; fi; echo i$i; done", "i1\ni3\n", 0),
 ("for f in /etc/p*; do echo F:$f; done", "F:/etc/passwd\nF:/etc/profile\n", 0),
 ("for a in $(seq 1 3); do for b in x y; do echo $a$b; done; done", "1x\n1y\n2x\n2y\n3x\n3y\n", 0),
 ("echo 'a b c' | while read x y; do echo x=$x y=$y; done", "x=a y=b c\n", 1),
 ("printf 'l1\\nl2\\n' > /tmp/in.txt; while read l; do echo got $l; done < /tmp/in.txt", "got l1\ngot l2\n", 1),
 ("printf 'p\\nq\\n' | while read l; do echo piped $l; done", "piped p\npiped q\n", 1),
 ("if test 1 -eq 1; then echo yes; fi; echo end", "yes\nend\n", 0),
 ("FOO=bar; echo $FOO", "bar\n", 0),
 ("FOO=temp true; echo \"[$FOO]\"", "[bar]\n", 0),
 ("echo \"${NOPE:-dflt} ${#PATH}\"", "dflt 13\n", 0),
 ("echo $((2*3+1)) $((FOO2=1))", None, None),
 ("printf 'echo ${1:-none} ${2:-two}\\n' > /tmp/d.sh; sh /tmp/d.sh alpha", "alpha two\n", 0),
 ("n=4; echo $((n*2+1))", "9\n", 0),
 ("echo $$ | wc -w", "1 \n", 0),
 ("echo hi # a comment; echo no", "hi\n", 0),
 ("echo a#b", "a#b\n", 0),
 ("echo \\$HOME", "$HOME\n", 0),
 ("echo \"\\$HOME\"", "$HOME\n", 0),
 ("echo hi > /dev/null; echo x", "x\n", 0),
 ("ls | wc -l", None, 0),
 ("ls /etc/passwd", "/etc/passwd\n", 0),
 ("ls -1 /etc | head -2", "group\nhostname\n", 0),
 ("which greet", "/bin/greet\n", 0),
 ("type greet; type echo; type nosuchthing", "greet is /bin/greet\necho is a shell builtin\ntype: nosuchthing: not found\n", 1),
 ("greet", "Hello from /bin/greet\n", 0),
 ("cat /proc/mounts | head -1", "/dev/hda / ext2 rw 0 0\n", 0),
 ("stat /proc/version | head -2", None, 0),
 ("cat /proc/filesystems | wc -l", "3 \n", 0),
 # scripts: arguments, shift, exit status, pipes, redirects
 ("printf 'echo \"n=$# 1=$1 all=$@\"\\nshift\\nfor a in \"$@\"; do echo arg:$a; done\\nexit 7\\necho not\\n' > /tmp/a.sh; chmod 755 /tmp/a.sh; /tmp/a.sh foo \"bar baz\"; echo st=$?", "n=2 1=foo all=foo bar baz\narg:bar baz\nst=7\n", 0),
 ("/tmp/a.sh x | wc -l", "1 \n", 0),
 ("/tmp/a.sh x > /tmp/script.out; cat /tmp/script.out | wc -l", "1 \n", 0),
 ("printf 'no newline' > /tmp/nn; cat /tmp/nn | wc -c", "10 \n", 0),
 ("cat /tmp/nn /tmp/nn | wc -c", "20 \n", 0),
 ("printf 'echo $0 $1\\n' > /tmp/s2.sh; source /tmp/s2.sh hi", "/tmp/s2.sh hi\n", 0),
 ("printf 'false\\n' > /tmp/s3.sh; chmod 755 /tmp/s3.sh; /tmp/s3.sh || echo failed", "failed\n", 0),
 # new tools
 ("printf 'a\\nb\\nc\\nd\\n' > /tmp/x1; printf 'a\\nB\\nc\\nd\\ne\\n' > /tmp/x2; diff /tmp/x1 /tmp/x2", "2c2\n< b\n---\n> B\n4a5\n> e\n", 1),
 ("diff /tmp/x1 /tmp/x1", "", 0),
 ("cmp /tmp/x1 /tmp/x2", "/tmp/x1 /tmp/x2 differ: byte 3, line 2\n", 1),
 ("tac /tmp/x1 | tr '\\n' ' '", "d c b a ", 0),
 ("echo abc | rev", "cba\n", 0),
 ("echo hello world | sed 's/o/0/g'", "hell0 w0rld\n", 0),
 ("sed -n '2,3p' /tmp/x1", "b\nc\n", 0),
 ("sed '$d' /tmp/x1 | tr '\\n' ' '", "a b c ", 0),
 ("nl /tmp/x1 | head -1", "     1\ta\n", 0),
 ("echo 'one two three four five' | fold -w 8", "one two \nthree fo\nur five\n", 0),
 ("paste /tmp/x1 /tmp/x2 | head -2", "a\ta\nb\tB\n", 0),
 ("file /etc/passwd /bin /bin/greet", "/etc/passwd: ASCII text\n/bin: directory\n/bin/greet: script text\n", 0),
 ("realpath /etc/../tmp/./x1", "/tmp/x1\n", 0),
 ("mktemp | wc -w", "1 \n", 0),
 ("ln -s /etc/passwd /tmp/lnk; readlink /tmp/lnk", "/etc/passwd\n", 0),
 ("mount | head -1", "/dev/hda on / type ext2 (rw)\n", 0),
 ("printenv HOME", "/home/guest\n", 0),
 ("whoami; su root; whoami", "guest\nroot\n", 0),
]

script = "".join(c + "\n" for c, _, _ in T)
out = subprocess.run([host, work], input=script.encode(), capture_output=True, timeout=120).stdout
out = out.decode("latin-1").replace("\r", "")
body = out.split("\x1eREADY\n", 1)[1]
chunks = body.split("\x1e")
# chunks[0] is output of cmd 0 followed by "<rc>\n" at the start of chunks[1], etc.
fails = 0
text, rcs = [], []
cur = chunks[0]
for ch in chunks[1:]:
    nl = ch.index("\n")
    rcs.append(int(ch[:nl])); text.append(cur); cur = ch[nl + 1:]
if len(text) != len(T):
    print("expected %d results, got %d" % (len(T), len(text))); sys.exit(2)
for (cmd, want, wrc), got, rc in zip(T, text, rcs):
    ok = True
    if want is not None and got != want: ok = False
    if wrc is not None and rc != wrc: ok = False
    if not ok:
        fails += 1
        print("FAIL: %s\n   want %r rc=%s\n   got  %r rc=%d" % (cmd, want, wrc, got, rc))
print("%d/%d shell tests passed" % (len(T) - fails, len(T)))
shutil.rmtree(tmp)
sys.exit(1 if fails else 0)
