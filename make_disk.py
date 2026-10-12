import os
import struct
import subprocess

SECTOR_SIZE = 512
TOTAL_SECTORS = 32768       # 16 MiB

# First 1024 sectors are reserved for the BIOS boot area + kernel.
# The ext2 volume starts at LBA 1024, so the kernel can never collide
# with filesystem allocations (the ext2 superblock lands at volume
# offset 1024 == disk LBA 1026 and stays clear of the boot area).
RESERVED = 1024

KERNEL_LBA = 16
STAGE2_LBA = 1
STAGE2_SECTORS = 16

image = bytearray(TOTAL_SECTORS * SECTOR_SIZE)

with open("bootsector.bin", "rb") as f:
    boot = bytearray(f.read())

# Accept either a raw <=510-byte bootloader or an already-padded
# 512-byte boot sector.
if len(boot) > 512:
    raise RuntimeError(
        f"bootsector.bin is too large: {len(boot)} bytes (max 512)"
    )

if len(boot) == 510:
    boot += b"\x55\xaa"
elif len(boot) < 510:
    boot = boot.ljust(510, b"\x00") + b"\x55\xaa"

if len(boot) != 512:
    raise RuntimeError("internal boot sector padding failed")

image[0:SECTOR_SIZE] = boot

# Keep bootsector.bin itself synchronized with the image.
with open("bootsector.bin", "wb") as f:
    f.write(boot)

with open("stage2.bin", "rb") as f:
    stage2 = bytearray(f.read())

assert len(stage2) <= STAGE2_SECTORS * SECTOR_SIZE, \
    "stage2.bin grew beyond its reserved bootloader area"

with open("starlightos64.bin", "rb") as f:
    kernel = f.read()

kernel_sectors = (len(kernel) + SECTOR_SIZE - 1) // SECTOR_SIZE

assert KERNEL_LBA + kernel_sectors <= RESERVED, \
    "x86_64 kernel no longer fits in the reserved boot area"
# stage2 loads the kernel to 0x10000 in real mode first; that buffer ends at 0x90000.
assert len(kernel) <= 0x80000, "kernel exceeds the 512 KiB real-mode load buffer"

def symbol(name):
    output = subprocess.check_output(
        ["x86_64-elf-nm", "-n", "starlightos64.elf"],
        text=True
    )
    for line in output.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    raise RuntimeError("symbol not found: " + name)

bss_start = symbol("__bss_start")
bss_end = symbol("__bss_end")

def patch_after(tag, value, fmt):
    marker = tag.encode("ascii")
    pos = stage2.find(marker)
    if pos < 0:
        raise RuntimeError("stage2 marker missing: " + tag)

    size = struct.calcsize(fmt)
    start = pos + len(marker)
    stage2[start:start + size] = struct.pack(fmt, value)

patch_after("KSEC", kernel_sectors, "<H")
patch_after("KLBA", KERNEL_LBA, "<I")
patch_after("KBYT", len(kernel), "<I")
patch_after("BSSS", bss_start, "<I")
patch_after("BSSE", bss_end, "<I")

# Put stage2 immediately after the boot sector.
stage2_start = STAGE2_LBA * SECTOR_SIZE
image[stage2_start:stage2_start + len(stage2)] = stage2

# Put the raw kernel at its fixed LBA.
kernel_start = KERNEL_LBA * SECTOR_SIZE
image[kernel_start:kernel_start + len(kernel)] = kernel

# ---------------------------------------------------------------------
# Sample content: everything in disk_files/ plus generated WAVs.
import math, wave, io

def synth_wav(notes, rate=22050, wave_kind='sine', gain=0.55):
    """notes: list of (freq_hz or 0, seconds). Returns WAV bytes (16-bit mono)."""
    samples = bytearray()
    for freq, secs in notes:
        n = int(rate * secs)
        for i in range(n):
            t = i / rate
            env = min(1.0, i / (rate * 0.01), (n - i) / (rate * 0.06))
            if freq == 0:
                v = 0.0
            elif wave_kind == 'sine':
                v = math.sin(2 * math.pi * freq * t)
            else:
                v = 2 * ((freq * t) % 1.0) - 1.0
            samples += struct.pack('<h', int(32767 * gain * env * v))
    buf = io.BytesIO()
    with wave.open(buf, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate); w.writeframes(bytes(samples))
    return buf.getvalue()

def stereo_pan_wav(freq_l, freq_r, secs, rate=22050):
    out = bytearray()
    n = int(rate * secs)
    for i in range(n):
        t = i / rate
        env = min(1.0, i / (rate * 0.02), (n - i) / (rate * 0.1))
        out += struct.pack('<hh', int(20000 * env * math.sin(2 * math.pi * freq_l * t)),
                                  int(20000 * env * math.sin(2 * math.pi * freq_r * t)))
    buf = io.BytesIO()
    with wave.open(buf, 'wb') as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate); w.writeframes(bytes(out))
    return buf.getvalue()

# Root files, in the order they will appear in the root directory.
# Guests own the root directory and its plain files; MINES.ELF is 0755.
UID_GUEST, GID_GUEST = 1000, 1000
F_MODE = 0o644
X_MODE = 0o755
E_MODE = 0o444                      # etc files: world-readable, root-owned
D_MODE = 0o755
STICKY = 0o1777

root_files = [
    ('HELLO.TXT',
        b'Hello from StarlightOS!\n'
        b'This file came from an ext2 disk.\n', F_MODE),
    ('README.TXT',
        b'StarlightOS ext2 storage is working.\n'
        b'ATA PIO -> block device -> ext2.\n', F_MODE),
    ('KERNEL.TXT',
        b'x86_64 BIOS boot.\n'
        b'Running through the StarlightOS bootloader.\n', F_MODE),
    ('MINES.ELF',
        open(os.path.join(os.path.dirname(__file__), 'apps', 'MINES.ELF'), 'rb').read(),
        X_MODE),
]

# Optional newlib-linked programs: any apps/*.ELF other than MINES.ELF that has been built
# (e.g. CDEMO.ELF via `make apps/CDEMO.ELF`) is installed in the root directory, mode 0755.
_apps_dir = os.path.join(os.path.dirname(__file__), 'apps')
for _n in sorted(os.listdir(_apps_dir)):
    if _n.upper().endswith('.ELF') and _n != 'MINES.ELF':
        root_files.append((_n, open(os.path.join(_apps_dir, _n), 'rb').read(), X_MODE))

font_path = os.path.join(
    os.path.dirname(__file__),
    'assets',
    'JetBrainsMonoNerdFont-Regular-ASCII.ttf'
)

if os.path.isfile(font_path):
    with open(font_path, 'rb') as f:
        root_files.append(('FONT.TTF', f.read(), F_MODE))

C5, E5, G5, C6 = 523.25, 659.25, 783.99, 1046.5
root_files.append(('CHIME.WAV', synth_wav([(C5, 0.12), (E5, 0.12), (G5, 0.12), (C6, 0.45)]), F_MODE))
root_files.append(('STEREO.WAV', stereo_pan_wav(440.0, 660.0, 1.2), F_MODE))   # 440 Hz left, 660 Hz right
root_files.append(('TONE8.WAV', None, F_MODE))                                 # placeholder replaced below

# 8-bit 8 kHz mono test tone (exercises the 8-bit PCM + resampling path)
r8 = 8000
pcm8 = bytes(int(128 + 100 * math.sin(2 * math.pi * 1000 * i / r8)) for i in range(r8 // 2))
buf = io.BytesIO()
with wave.open(buf, 'wb') as w:
    w.setnchannels(1); w.setsampwidth(1); w.setframerate(r8); w.writeframes(pcm8)
root_files[-1] = ('TONE8.WAV', buf.getvalue(), F_MODE)

disk_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'disk_files')
if os.path.isdir(disk_dir):
    for fname in sorted(os.listdir(disk_dir)):
        if fname.startswith('.'):
            continue
        with open(os.path.join(disk_dir, fname), 'rb') as f:
            root_files.append((fname, f.read(), F_MODE))

# ---------------------------------------------------------------------
# Unix FHS tree inside the root filesystem.
#
#   /            guest:guest  0755   (guest can create files at the root)
#   /bin         root:root   0755
#   /etc         root:root   0755    /etc/passwd /etc/group /etc/hostname
#                                    /etc/profile  (all 0444 root:root)
#   /home        root:root   1777
#   /home/guest  guest:guest 0755    ~/.profile
#   /tmp         root:root   1777
#   /var         root:root   0755
#   /usr         root:root   0755
#   /usr/bin     root:root   0755    /usr/bin/up
#   /dev         root:root   0755    mount point for devfs
#   /latest -> HELLO.TXT
#
# /proc lives entirely in procfs (mounted at boot), not on the disk.
# All root_files above live at / (existing tests and AUTOEXEC.SH read them).

ETC_CONTENTS = {
    'passwd':
        b'root:x:0:0:root:/root:/bin/sh\n'
        b'guest:x:1000:1000:guest:/home/guest:starlightsh\n',
    'group':
        b'root:x:0:\n'
        b'guest:x:1000:\n',
    'hostname':
        b'starlight\n',
    'profile':
        b'# System-wide profile, sourced for every login.\n'
        b'export PATH=/bin:/usr/bin\n'
        b'export GROUP=users\n'
        b'echo "Welcome to StarlightOS."\n',
}

HOME_PROFILE = (
    b'# Personal profile for guest.\n'
    b'export HOME_ALIAS=set\n'
    b'alias h=' + b"'cat /etc/hostname'" + b'\n'
)

GREET_SH = (
    b'#!/bin/sh\n'
    b'# An executable script kept in /bin (found via $PATH).\n'
    b'echo Hello from /bin/greet\n'
)

UP_SH = (
    b'#!/bin/sh\n'
    b'echo up from /usr/bin\n'
)

LATEST_TARGET = 'HELLO.TXT'

# /home/guest/httpd_test: the user's chat-room folder. `cd ~/httpd_test; httpd`
# serves INDEX.HTM and turns on the live chat API (see httpd.c).
_here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(_here, 'user_files', 'guest', 'httpd_test', 'INDEX.HTM'), 'rb') as f:
    _chat_index = f.read()
HTTPD_TEST_FILES = {
    'INDEX.HTM': _chat_index,
    'README.TXT':
        b'StarlightOS live chat\n'
        b'=====================\n'
        b'  cd /home/guest/httpd_test\n'
        b'  httpd 8080\n'
        b'then browse to http://<guest-ip>:8080/  (make run-chat forwards it to http://localhost:8080/).\n'
        b'Every browser tab is a user; messages are relayed in real time over Server-Sent Events.\n',
}

# ---------------------------------------------------------------------
# ext2 volume builder.
#
# Layout (4 KiB blocks, one block group, 128-byte inodes):
#   superblock      volume block 0   (at byte offset 1024 of the volume)
#   group descriptor table          volume block 1
#   block bitmap                    volume block 2
#   inode bitmap                    volume block 3
#   inode table                     volume blocks 4..7
#   root directory data             volume block 8
#   file data                        volume block 9 onward
EXT2_BASE_LBA = 1024        # filesystem starts right after the boot area
BLOCK_SIZE = 4096
VOLUME_BLOCKS = (TOTAL_SECTORS - EXT2_BASE_LBA) * SECTOR_SIZE // BLOCK_SIZE
INODES_COUNT = 128
INODES_PER_GROUP = 128
BLOCKS_PER_GROUP = 8192
FIRST_INO = 11
FIRST_DATA_BLOCK = 0
ROOT_INO = 2
INODE_SIZE = 128
FEATURE_INCOMPAT_FILETYPE = 0x0002

BLK_SUPER = 0
BLK_GDT = 1
BLK_BLOCK_BITMAP = 2
BLK_INODE_BITMAP = 3
BLK_INODE_TABLE = 4          # 128 inodes * 128 B = blocks 4..7
BLK_ROOT_DIR = 8

volume_base = EXT2_BASE_LBA * SECTOR_SIZE
vol = bytearray(VOLUME_BLOCKS * BLOCK_SIZE)

def p32(buf, off, v): struct.pack_into('<I', buf, off, v)
def p16(buf, off, v): struct.pack_into('<H', buf, off, v)
def p8(buf, off, v): struct.pack_into('<B', buf, off, v)

def align4(n): return (n + 3) & ~3

# ---- bitmaps ---------------------------------------------------------
bb = bytearray(BLOCK_SIZE)    # block bitmap
ib = bytearray(BLOCK_SIZE)    # inode bitmap

# Metadata blocks 0..8 are already allocated.
for blk in range(BLK_ROOT_DIR + 1):
    bb[blk >> 3] |= 1 << (blk & 7)

# Reserve inodes 1..10 (as the kernel does: first_ino = 11).
for ino in range(1, FIRST_INO):
    ib[(ino - 1) >> 3] |= 1 << ((ino - 1) & 7)

next_data_block = BLK_ROOT_DIR + 1

def alloc_data_block():
    global next_data_block
    b = next_data_block
    next_data_block += 1
    bb[b >> 3] |= 1 << (b & 7)
    return b

def mark_inode(ino):
    ib[(ino - 1) >> 3] |= 1 << ((ino - 1) & 7)

# ---- inode table ------------------------------------------------------
inode_table = bytearray(4 * BLOCK_SIZE)

def write_inode(ino, mode, size, links, sectors, ptrs, uid, gid):
    """sectors = i_blocks (512-byte units); ptrs = i_block[15] array."""
    off = (ino - 1) * INODE_SIZE
    p16(inode_table, off + 0, mode)
    p16(inode_table, off + 2, uid)
    p32(inode_table, off + 4, size)
    p32(inode_table, off + 8, 0)              # atime
    p32(inode_table, off + 12, 0)             # ctime
    p32(inode_table, off + 16, 0)             # mtime
    p32(inode_table, off + 20, 0)             # dtime
    p16(inode_table, off + 24, gid)
    p16(inode_table, off + 26, links)
    p32(inode_table, off + 28, sectors)
    p32(inode_table, off + 32, 0)             # flags
    for i in range(15):
        p32(inode_table, off + 40 + i * 4, ptrs[i] if i < len(ptrs) else 0)

# ---- file data ---------------------------------------------------------
def store_file(data):
    """Allocates data blocks (+ indirect blocks as needed) for `data` and
    returns (direct_ptr_ptrs, sectors). Files here are < 4 MB so single
    indirect covers everything beyond the 12 direct blocks."""
    n = (len(data) + BLOCK_SIZE - 1) // BLOCK_SIZE
    dblocks = [alloc_data_block() for _ in range(n)]
    sectors = n
    ptrs = []

    for i, b in enumerate(dblocks):
        chunk = data[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE]
        vol[b * BLOCK_SIZE:b * BLOCK_SIZE + len(chunk)] = chunk

    direct = min(12, n)
    for i in range(direct):
        ptrs.append(dblocks[i])

    if n > 12:
        # single indirect block: a fresh block holding the rest of the pointers
        ind = alloc_data_block()
        sectors += 1
        ind_data = bytearray(BLOCK_SIZE)
        for i, b in enumerate(dblocks[12:]):
            p32(ind_data, i * 4, b)
        vol[ind * BLOCK_SIZE:(ind + 1) * BLOCK_SIZE] = ind_data
        ptrs.append(ind)

    return ptrs, sectors

# ---- directory tree ------------------------------------------------------
# Each entry of a directory is (name, kind) where kind is 'd' (dir), 'f'
# (file), or 'l' (symlink).  Names are real ext2 filenames (uppercase for
# files, lowercase for system dirs, like a real Unix box).
MODE_FS_DIR = 0x4000
MODE_FS_REG = 0x8000
MODE_FS_LNK = 0xA000
FT_DIR, FT_FILE, FT_LNK = 2, 1, 7

# The trusted-root bundle, installed at /etc/ssl/certs/ca-certificates.crt so
# https:// works out of the box: the kernel TLS client (cmd_net.c fetch) and
# libslweb's browser both load that path by default.  It is the Mozilla CA
# bundle shipped by the `certifi` project (MPL-2.0); refresh it with:
#   python3 -c "import certifi,shutil; shutil.copyfile(certifi.where(),'assets/ca-certificates.crt')"
_ca_bundle_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               'assets', 'ca-certificates.crt')
CA_BUNDLE = open(_ca_bundle_path, 'rb').read() if os.path.isfile(_ca_bundle_path) else None

tree = {
    '/': [
        ('bin', 'd'), ('dev', 'd'), ('etc', 'd'), ('home', 'd'), ('tmp', 'd'),
        ('var', 'd'), ('usr', 'd'),
    ] + [(name, 'f') for name, _, _ in root_files] +
        [('latest', 'l')],
    '/bin':     [('greet', 'f')],
    '/etc':     [(name, 'f') for name in ETC_CONTENTS],
    '/home':    [('guest', 'd')],
    '/tmp':     [],
    '/var':     [],
    '/usr':     [('bin', 'd')],
    '/usr/bin': [('up', 'f')],
    '/home/guest': [('.profile', 'f'), ('httpd_test', 'd')],
    '/home/guest/httpd_test': [('INDEX.HTM', 'f'), ('README.TXT', 'f')],
    '/dev':     [],                             # filled by devfs at boot
}

if CA_BUNDLE is not None:
    tree['/etc'].append(('ssl', 'd'))
    tree['/etc/ssl'] = [('certs', 'd')]
    tree['/etc/ssl/certs'] = [('ca-certificates.crt', 'f')]

def dir_owner(kind, path, name):
    """Returns (uid, gid, mode) for a directory entry by its location."""
    R = (0, 0)
    if kind == 'd':
        if path == '/':
            # The root itself is guest-owned so `echo x > T1.TXT` works.
            return (UID_GUEST, GID_GUEST, D_MODE)
        if path in ('/home/guest', '/home/guest/httpd_test'):
            return (UID_GUEST, GID_GUEST, D_MODE)
        if path in ('/home', '/tmp'):
            return R + (STICKY,)
        return R + (D_MODE,)
    if kind == 'l':
        return (UID_GUEST, GID_GUEST, 0)
    # regular file
    if path == '/etc' or path.startswith('/etc/'):
        return R + (E_MODE,)                    # /etc is root-owned, world-readable
    if path == '/home/guest' and name == '.profile':
        return (UID_GUEST, GID_GUEST, F_MODE)
    if name.upper().endswith('.ELF'):
        return (UID_GUEST, GID_GUEST, X_MODE)
    if path in ('/bin', '/usr/bin') and kind == 'f':
        return R + (X_MODE,)                    # executable scripts
    return (UID_GUEST, GID_GUEST, F_MODE)

def file_data(path, name):
    """Returns the bytes for file `name` at `path`, or None for a symlink."""
    if path == '/':
        for n, d, _ in root_files:
            if n == name:
                return d
    if path == '/etc':
        return ETC_CONTENTS[name]
    if path == '/etc/ssl/certs' and name == 'ca-certificates.crt':
        return CA_BUNDLE
    if path == '/home/guest' and name == '.profile':
        return HOME_PROFILE
    if path == '/home/guest/httpd_test':
        return HTTPD_TEST_FILES[name]
    if path == '/bin' and name == 'greet':
        return GREET_SH
    if path == '/usr/bin' and name == 'up':
        return UP_SH
    if path == '/' and name == 'latest':
        return None
    raise RuntimeError(f'unknown file {path}/{name}')

# Allocate directory inodes (root first, then subdirs in tree order).
next_ino = ROOT_INO                       # 2
dir_ino = {}
for path in tree:
    dir_ino[path] = next_ino
    mark_inode(next_ino)
    next_ino += 1

# Health check: the kernel reserves inodes 1..10; we use 2..10 and let
# the allocator at runtime (which starts scanning at s_first_ino = 11)
# see the bitmap correctly.  Dirs land in 2..11, files at 12+.
assert next_ino <= 24, next_ino      # bitmap is authoritative; runtime allocator scans it

# Files and symlinks get the remaining inodes.
file_inodes = {}          # (path, name) -> (ino, kind, data, mode, uid, gid)
for path, entries in tree.items():
    for name, kind in entries:
        if kind == 'd':
            continue                             # dirs got their inodes above
        if kind == 'f':
            data = file_data(path, name)
            assert data is not None
        else:
            data = None
        uid, gid, mode = dir_owner(kind, path, name)
        ino = next_ino
        next_ino += 1
        mark_inode(ino)
        file_inodes[(path, name)] = (ino, kind, data, mode, uid, gid)

def dir_path(path):
    if path == '/':
        return ''
    return os.path.dirname(path)

def nlink_dir(path):
    """A directory's link count = 2 + number of immediate subdirs."""
    n = 2
    for _, kind in tree[path]:
        if kind == 'd':
            n += 1
    return n

# ---- directory data blocks ------------------------------------------------
def put_dirent(buf, off, ino, rec_len, name, ftype):
    p32(buf, off, ino)
    p16(buf, off + 4, rec_len)
    buf[off + 6] = len(name)
    buf[off + 7] = ftype
    buf[off + 8:off + 8 + len(name)] = name

def write_dir_block(path, blk):
    """Writes the directory data for `path` into volume block `blk` and
    returns the inode metadata for it."""
    self_ino = dir_ino[path]
    par = dir_path(path)
    parent_ino = ROOT_INO if path == '/' else dir_ino[par]
    entries = tree[path]

    data = bytearray(BLOCK_SIZE)
    put_dirent(data, 0, self_ino, 12, b'.', FT_DIR)
    put_dirent(data, 12, parent_ino, 12, b'..', FT_DIR)
    off = 24
    last = -1
    for name, kind in entries:
        if kind == 'f':
            e_ino, _, _, _, _, _ = file_inodes[(path, name)]
            ftype = FT_FILE
        elif kind == 'l':
            e_ino, _, _, _, _, _ = file_inodes[(path, name)]
            ftype = FT_LNK
        else:
            e_ino = dir_ino[os.path.join(path, name)]
            ftype = FT_DIR
        l = align4(8 + len(name.encode('ascii')))
        put_dirent(data, off, e_ino, l, name.encode('ascii'), ftype)
        last = off
        off += l
    # Extend the final entry's rec_len to the end of the block so the
    # kernel's dir_add() can split it when new files are created at runtime.
    end_off = last + 4 if last >= 0 else 12 + 4      # last entry, or '..'
    p16(data, end_off, BLOCK_SIZE - (end_off - 4))

    vol[blk * BLOCK_SIZE:(blk + 1) * BLOCK_SIZE] = data

    uid, gid, mode = dir_owner('d', path, os.path.basename(path))
    return (MODE_FS_DIR | mode), nlink_dir(path), UID_GUEST if False else uid, gid

# Write the root directory into its fixed block, then every subdir.
for path in tree:
    if path == '/':
        blk = BLK_ROOT_DIR
    else:
        blk = alloc_data_block()
    mode, nlink, uid, gid = write_dir_block(path, blk)
    write_inode(dir_ino[path], mode, BLOCK_SIZE, nlink, 8, [blk], uid, gid)

# ---- regular files and symlinks --------------------------------------------
EXT2_S_IFLNK = 0xA000
for (path, name), (ino, kind, data, mode, uid, gid) in file_inodes.items():
    off = (ino - 1) * INODE_SIZE
    if kind == 'l':                                # fast symlink
        target = LATEST_TARGET.encode('ascii')
        write_inode(ino, MODE_FS_LNK | 0x1FF, len(target), 1, 0, [],
                    uid, gid)
        inode_table[off + 40:off + 40 + len(target)] = target
    else:
        if data is None:
            raise RuntimeError(f'file {path}/{name} has None data (kind={kind})')
        ptrs, sectors = store_file(data)
        write_inode(ino, MODE_FS_REG | mode, len(data), 1, sectors, ptrs, uid, gid)

# ---- superblock (byte offset 1024 inside volume block 0) ---------------
sb = bytearray(1024)
p32(sb, 0, INODES_COUNT)          # s_inodes_count
p32(sb, 4, VOLUME_BLOCKS)         # s_blocks_count
p32(sb, 8, 0)                     # s_r_blocks_count
p32(sb, 12, 0)                    # s_free_blocks_count (finalised below)
p32(sb, 16, 0)                    # s_free_inodes_count (finalised below)
p32(sb, 20, FIRST_DATA_BLOCK)     # s_first_data_block
p32(sb, 24, 2)                    # s_log_block_size -> 4096
p32(sb, 28, 2)                    # s_log_frag_size
p32(sb, 32, BLOCKS_PER_GROUP)
p32(sb, 36, BLOCKS_PER_GROUP)     # s_frags_per_group
p32(sb, 40, INODES_PER_GROUP)
p32(sb, 44, 0)                    # s_mtime
p32(sb, 48, 0)                    # s_wtime
p16(sb, 52, 0)                    # s_mnt_count
p16(sb, 54, 0xFFFF)               # s_max_mnt_count
p16(sb, 56, 0xEF53)               # s_magic
p16(sb, 58, 1)                    # s_state = clean
p16(sb, 60, 1)                    # s_errors = continue
p16(sb, 62, 0)                    # s_minor_rev_level
p32(sb, 64, 0)                    # s_lastcheck
p32(sb, 68, 0)                    # s_checkinterval
p32(sb, 72, 0)                    # s_creator_os = linux
p32(sb, 76, 1)                    # s_rev_level = 1 (dynamic inode size)
p16(sb, 80, 0)                    # s_def_resuid
p16(sb, 82, 0)                    # s_def_resgid
p32(sb, 84, FIRST_INO)            # s_first_ino
p16(sb, 88, INODE_SIZE)           # s_inode_size
p16(sb, 90, 0)                    # s_block_group_nr
p32(sb, 92, 0)                    # s_feature_compat
p32(sb, 96, FEATURE_INCOMPAT_FILETYPE)  # s_feature_incompat
p32(sb, 100, 0)                   # s_feature_ro_compat
p32(sb, 108, 0)                   # s_algorithm_usage_bitmap
p8(sb, 124, 0)                    # s_prealloc_blocks
p8(sb, 125, 0)                    # s_prealloc_dir_blocks
vol[BLK_SUPER * BLOCK_SIZE + 1024:BLK_SUPER * BLOCK_SIZE + 2048] = sb

# ---- group descriptor --------------------------------------------------
gdt = bytearray(32)
p32(gdt, 0, BLK_BLOCK_BITMAP)
p32(gdt, 4, BLK_INODE_BITMAP)
p32(gdt, 8, BLK_INODE_TABLE)
p16(gdt, 12, 0)                   # bg_free_blocks_count (finalised below)
p16(gdt, 14, 0)                   # bg_free_inodes_count (finalised below)
p16(gdt, 16, len(tree))           # bg_used_dirs_count = root + all subdirs
vol[BLK_GDT * BLOCK_SIZE:BLK_GDT * BLOCK_SIZE + 32] = gdt

# ---- free counts ----------------------------------------------------------
used_inodes = 0
for byte in ib:
    used_inodes += bin(byte).count('1')
free_inodes = INODES_COUNT - used_inodes
used_blocks = next_data_block                     # all blocks data was placed at
free_blocks = VOLUME_BLOCKS - used_blocks

p32(sb, 12, free_blocks)
p32(sb, 16, free_inodes)
vol[BLK_SUPER * BLOCK_SIZE + 1024:BLK_SUPER * BLOCK_SIZE + 2048] = sb
p16(gdt, 12, free_blocks)
p16(gdt, 14, free_inodes)
vol[BLK_GDT * BLOCK_SIZE:BLK_GDT * BLOCK_SIZE + 32] = gdt

# ---- splice the volume into the disk image --------------------------------
vol[BLK_BLOCK_BITMAP * BLOCK_SIZE:(BLK_BLOCK_BITMAP + 1) * BLOCK_SIZE] = bb
vol[BLK_INODE_BITMAP * BLOCK_SIZE:(BLK_INODE_BITMAP + 1) * BLOCK_SIZE] = ib
vol[BLK_INODE_TABLE * BLOCK_SIZE:(BLK_INODE_TABLE + 4) * BLOCK_SIZE] = inode_table

image[volume_base:volume_base + len(vol)] = vol

with open("disk.img", "wb") as f:
    f.write(image)

print("Created disk.img (16 MiB ext2 data disk)")
print("  stage2 LBA:", STAGE2_LBA)
print("  kernel LBA:", KERNEL_LBA)
print("  kernel:", len(kernel), "bytes,", kernel_sectors, "sectors")
print("  BSS:", hex(bss_start), "-", hex(bss_end))
print("  ext2 volume at LBA", EXT2_BASE_LBA, ":", VOLUME_BLOCKS, "blocks of", BLOCK_SIZE)
print("Directories:")
for path in tree:
    print("  ", path or '/', "(inode", str(dir_ino[path]) + ")")
print("Files and links:")
for (path, name), (ino, kind, data, mode, uid, gid) in file_inodes.items():
    extra = '(symlink -> %s)' % LATEST_TARGET if kind == 'l' else f'{len(data)} bytes'
    print("   ", os.path.join(path, name), extra, '(inode', str(ino) + ')')