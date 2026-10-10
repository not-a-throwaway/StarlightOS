# StarlightOS feature tour:  source DEMO.SH
echo "== system =="
neofetch
echo "== files, pipes and redirection =="
ls -l | head -n 5
echo "hello from a script" > NOTE.TXT
cat NOTE.TXT | grep -i HELLO
rm NOTE.TXT
echo "== network =="
ip
echo "== sound =="
sound
