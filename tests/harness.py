"""QEMU test harness for StarlightOS: boot a copy of disk.img, type into the shell, read the serial console."""
import json, os, re, shutil, socket, struct, subprocess, tempfile, time

KEYMAP = {' ': 'spc', '\n': 'ret', '\t': 'tab', '-': 'minus', '=': 'equal', '.': 'dot', ',': 'comma', '/': 'slash',
          ':': 'shift-semicolon', ';': 'semicolon', '_': 'shift-minus', '>': 'shift-dot', '<': 'shift-comma',
          '"': 'shift-apostrophe', "'": 'apostrophe', '|': 'shift-backslash', '\\': 'backslash', '*': 'shift-8',
          '+': 'shift-equal', '?': 'shift-slash', '!': 'shift-1', '@': 'shift-2', '#': 'shift-3', '$': 'shift-4',
          '%': 'shift-5', '&': 'shift-7', '(': 'shift-9', ')': 'shift-0', '[': 'bracket_left', ']': 'bracket_right',
          '~': 'shift-grave_accent', '`': 'grave_accent', '{': 'shift-bracket_left', '}': 'shift-bracket_right', '^': 'shift-6'}

PROMPT = re.compile(r'guest@\S+:\S* [$] ')


class VM:
    def __init__(self, disk, workdir, audio=False, hostfwd='', mem='256M', display=None):
        self.work = workdir
        self.disk = os.path.join(workdir, 'disk.img')
        shutil.copyfile(disk, self.disk)
        self.serial = os.path.join(workdir, 'serial.log')
        self.audio_path = os.path.join(workdir, 'audio.wav')
        self.sock_path = os.path.join(workdir, 'qmp.sock')
        args = ['qemu-system-x86_64', '-m', mem, '-drive', f'format=raw,file={self.disk},if=ide,index=0',
                '-boot', 'order=c', '-display', 'none', '-vga', 'std', '-serial', f'file:{self.serial}',
                '-qmp', f'unix:{self.sock_path},server,nowait',
                '-netdev', f'user,id=net0{hostfwd}', '-device', 'rtl8139,netdev=net0']
        if audio:
            args += ['-audiodev', f'wav,id=snd0,path={self.audio_path}', '-device', 'AC97,audiodev=snd0']
        else:
            args += ['-audiodev', 'none,id=snd0', '-device', 'AC97,audiodev=snd0']
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        for _ in range(100):
            if os.path.exists(self.sock_path):
                break
            time.sleep(0.1)
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(self.sock_path)
        self.f = self.s.makefile('rw')
        self.f.readline()
        self._cmd('qmp_capabilities')

    def _cmd(self, name, **args):
        self.f.write(json.dumps({'execute': name, 'arguments': args}) + '\n')
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if 'return' in r or 'error' in r:
                return r

    def key(self, k):
        self._cmd('human-monitor-command', **{'command-line': 'sendkey ' + k})

    def type(self, text, delay=0.03):
        for ch in text:
            k = KEYMAP.get(ch) or (('shift-' + ch.lower()) if ch.isupper() else ch)
            self.key(k)
            time.sleep(delay)

    def log(self):
        try:
            return open(self.serial, errors='replace').read().replace('\r', '')
        except FileNotFoundError:
            return ''

    def wait_for(self, text, timeout=30, since=0):
        end = time.time() + timeout
        while time.time() < end:
            if text in self.log()[since:]:
                return True
            time.sleep(0.2)
        return False

    def boot(self, timeout=40):
        ok = self.wait_for('guest@', timeout)
        if ok:
            self.key('6')           # open the Terminal app (already focused in the shell by default)
        time.sleep(1.0)
        return ok

    def wait_lease(self, timeout=25):
        end = time.time() + timeout
        while time.time() < end:
            out = self.run('ip', timeout=8)
            if 'inet ' in out:
                return True
            time.sleep(1)
        return False

    def run(self, cmd, timeout=15, raw=False):
        """Types a command, waits for the next prompt, returns the output between echo and prompt."""
        start = len(self.log())
        self.type(cmd + '\n')
        end = time.time() + timeout
        while time.time() < end:
            seg = self.log()[start:]
            nl = seg.find('\n')
            if nl >= 0 and PROMPT.search(seg[nl:]):
                out = seg[nl + 1:]
                m = list(PROMPT.finditer(out))[-1]
                out = out[:m.start()]
                return out if raw else out.strip('\n')
            time.sleep(0.15)
        return '<<TIMEOUT>>' + self.log()[start:]

    def screenshot(self, path):
        from PIL import Image
        ppm = path + '.ppm'
        self._cmd('screendump', filename=ppm)
        time.sleep(0.3)
        Image.open(ppm).save(path)
        os.remove(ppm)

    def quit(self):
        try:
            self._cmd('quit')
        except Exception:
            pass
        try:
            self.proc.wait(timeout=8)
        except Exception:
            self.proc.kill()
        if getattr(self, 'audio_path', None):
            finalize_wav(self.audio_path)


def finalize_wav(path):
    """Patch the RIFF/data sizes QEMU leaves as placeholders.

    QEMU 11.1.x never runs the wav backend's fini callback (the AC97 device keeps
    the audio backend alive through shutdown), so the capture keeps the zero sizes
    written when the file was opened. The samples are all present; fix the header
    so `wave` can read it.
    """
    if not os.path.exists(path):
        return
    with open(path, 'rb') as f:
        data = bytearray(f.read())
    if len(data) < 44 or data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        return
    off = 12
    while off + 8 <= len(data):
        if data[off:off + 4] == b'data':
            struct.pack_into('<I', data, 4, len(data) - 8)
            struct.pack_into('<I', data, off + 4, len(data) - (off + 8))
            break
        chunk = struct.unpack_from('<I', data, off + 4)[0]
        off += 8 + chunk + (chunk & 1)
    with open(path, 'wb') as f:
        f.write(data)



# ----------------------------------------------------------------- ext2 reader for verifying files on the image
# Matches the layout make_disk.py emits: the filesystem starts at LBA 1024
# (EXT2_BASE_LBA), the superblock sits at volume byte offset 1024, and the
# volume uses 4096-byte blocks with one block group.
class Ext2Image:
    BASE_LBA = 1024

    def __init__(self, path):
        self.d = open(path, 'rb').read()
        self.base = self.BASE_LBA * 512
        sb = self.d[self.base + 1024:self.base + 2048]
        log = struct.unpack_from('<I', sb, 24)[0]
        self.bs = 1024 << log
        self.first_data_block = struct.unpack_from('<I', sb, 20)[0]
        self.inodes_per_group = struct.unpack_from('<I', sb, 40)[0]
        rev = struct.unpack_from('<I', sb, 76)[0]
        self.inode_size = struct.unpack_from('<H', sb, 88)[0] if rev >= 1 else 128
        gdt = self.d[self.base + (self.first_data_block + 1) * self.bs:
                     self.base + (self.first_data_block + 2) * self.bs]
        self.inode_table = struct.unpack_from('<I', gdt, 8)[0]

    def _le16(self, b, o): return struct.unpack_from('<H', b, o)[0]
    def _le32(self, b, o): return struct.unpack_from('<I', b, o)[0]

    def _block(self, n):
        return self.d[self.base + n * self.bs:self.base + (n + 1) * self.bs]

    def _ino(self, n):
        base = self.base + self.inode_table * self.bs + (n - 1) * self.inode_size
        return self.d[base:base + self.inode_size]

    def _file_blocks(self, inode):
        blocks = []
        for i in range(12):
            b = self._le32(inode, 40 + i * 4)
            if b: blocks.append(b)
        ind = self._le32(inode, 40 + 12 * 4)
        if ind:
            indb = self._block(ind)
            for i in range(0, self.bs, 4):
                b = self._le32(indb, i)
                if b: blocks.append(b)
        dind = self._le32(inode, 40 + 13 * 4)
        if dind:
            dindb = self._block(dind)
            for j in range(0, self.bs, 4):
                l1 = self._le32(dindb, j)
                if l1:
                    l1b = self._block(l1)
                    for i in range(0, self.bs, 4):
                        b = self._le32(l1b, i)
                        if b: blocks.append(b)
        return blocks

    def _dir_entries(self, inode):
        size = self._le32(inode, 4)
        blocks = self._file_blocks(inode)
        entries = []
        for bi in range((size + self.bs - 1) // self.bs):
            if bi >= len(blocks): continue
            blk = self._block(blocks[bi])
            off = 0
            while off + 8 <= self.bs:
                e = blk[off:off + 8]
                rec = self._le16(e, 4)
                if rec < 8 or off + rec > self.bs:
                    break
                ino = self._le32(e, 0)
                if ino != 0:
                    name = blk[off + 8:off + 8 + e[6]].decode('ascii', 'replace')
                    entries.append((name, ino, e[7]))
                off += rec
        return entries

    def _data(self, inode, size):
        out = b''
        for b in self._file_blocks(inode):
            out += self._block(b)
            if len(out) >= size: break
        return out[:size]

    def read(self, path):
        parts = [p for p in path.split('/') if p]
        ino = 2
        for idx, part in enumerate(parts):
            found = None
            for name, eino, _ftype in self._dir_entries(self._ino(ino)):
                if name == part:
                    found = eino
                    break
            if not found:
                return None
            ino = found
            if idx == len(parts) - 1:
                size = self._le32(self._ino(ino), 4)
                return self._data(self._ino(ino), size)
        return None
