# A tour of the unix-style shell.  Run it with:   source UNIXDEMO.SH   (or: sh UNIXDEMO.SH alpha beta)
echo "script=$0  args=$#  first=${1:-none}"

# variables, command substitution, arithmetic
name=$(echo starlight | tr a-z A-Z)
echo "name=$name  twice=$((2 * 21))"

# loops, globs and conditionals
for f in /etc/p*; do
  if test -f $f; then echo "file: $f"; fi
done
n=3
while test $n -gt 0; do echo "countdown $n"; n=$((n - 1)); done

# pipes, redirection, stderr
echo "one two three" | tr ' ' '\n' | tac > /tmp/words.txt
cat /tmp/words.txt
cat /no/such/file 2>/dev/null || echo "cat failed (status $?), error hidden by 2>/dev/null"
ls /bin /etc 2>&1 | head -3

# reading input from a file
while read line; do echo "read: $line"; done < /tmp/words.txt
