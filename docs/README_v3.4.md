> **Archived notes.** Written for v3.4, when the project was still called Myos; the name and
> file names below have been updated to StarlightOS, but sizes/behaviour describe v3.4 and
> some details (e.g. the 12x16 terminal cell, now 9 px wide) are out of date. See `../README.md`.

# StarlightOS

## Current build and architecture

Run `make run` to build and boot the x86_64 BIOS image in QEMU. Use
`make run32` for the 32-bit Multiboot build. The x86_64 startup sets
the SSE state, aligns the kernel stack, and maps the framebuffer's
physical address before entering the kernel.

The x86_64 kernel now has a real user-mode ELF64 path: apps run at ring
3 with their own user pages, a protected kernel stack, and a small
syscall interface. The desktop Minesweeper game is built as
`apps/MINES.ELF` and is no longer linked into the kernel. The current
process model is one foreground process at a time; there is no scheduler
or multitasking yet. The TTF loader now clears complete 16x16 glyphs,
including blank glyphs, before publishing them, fixing uninitialized
stack pixels that had painted noise across most text.
The desktop terminal now uses a compact 12x16 pixel character pitch
and starts at 80 columns. Its bundled font is a small JetBrains Mono
Nerd Font ASCII subset; GUI text also uses tighter native advances.

# StarlightOS — changelog

## Quick start (macOS / Apple Silicon)

Install the cross-compilers with Homebrew and QEMU with MacPorts:

```sh
brew install i686-elf-gcc x86_64-elf-gcc
sudo port install qemu
```

Then, from this directory, build and boot StarlightOS in QEMU:

```sh
export PATH="/opt/homebrew/bin:/opt/local/bin:$PATH"
make clean && make run
```

QEMU opens a window; close it to exit. `make` builds both kernels,
`make run` boots the x86_64 kernel from its BIOS disk image, and
`make run32` boots the 32-bit kernel through QEMU's Multiboot loader.
The x86_64 startup enables SSE, aligns the kernel stack, and maps the
full 4 GiB range used by the QEMU framebuffer.

## Architecture and desktop

The x86_64 kernel loads bounded static ELF64 executables from FAT16,
maps their pages at ring 3, and enters them with a TSS-provided kernel
stack. User pages are isolated from supervisor mappings; ELF permissions
are enforced with read-only text and non-executable data/stack pages.
The syscall ABI currently provides terminal output, keyboard input, and
exit. The process model is synchronous and single-process: no scheduler,
dynamic linker, or multitasking yet. Minesweeper's rules are compiled
only into the standalone user ELF. TTF glyphs preserve supersampled
coverage, with blank glyphs initialized correctly and descenders fitted
inside the cell.

The dock launches **Minesweeper** as a user-mode app in the Terminal.
Use `r x y` to reveal a tile, `f x y` to flag it, `n` for a new game, and
`q` to quit. Coordinates are zero-based. The first reveal is safe; the
game includes expanding empty areas, clues, flags, and win/loss states.
The shell also accepts `run MINES.ELF`.

Both cross-compilers are installed with Homebrew. MacPorts QEMU was already
installed on this Mac.

## This round: real 16x16 font (not just a bigger box), UI scaled to match

Historical changelog entry. The current build and verification status
are described above; older notes below record the state at the time each
change was made.

**Same disclosure as always: no cross-toolchain, no QEMU, no network.**
This round got more real verification than most, though: every file
was recompiled clean (`-Wall -Wextra`, substitute `-m32` toolchain,
zero warnings) after *every* edit, not just once at the end -- that
process caught a real bug immediately (a stray `*/` inside a comment
in a previous round's `vga.c` that silently broke the build). More
importantly, the new TTF rasterizer was actually run standalone on
the host, fed the real bundled `JetBrainsMonoNerdFont-Regular-ASCII.ttf`, and its
output printed as ASCII art -- see "How this was actually verified"
below. That's real evidence the glyphs are correct, not just that
the code compiles.

Asked for: "make the font loaded bigger for higher detail and
everything else bigger since it's HD 1920x1080." Two different
things were actually needed here, and both got done:

### The font itself: 16x16, not an upscaled 8x8

`font_ttf.c`'s whole rasterizer -- outline flattening, supersampling
(bumped 32x32 -> 64x64 to keep the same antialiasing ratio),
downsampling -- now runs at 16x16 output. That means a loaded TTF's
curves are genuinely resolved at higher detail, not just drawn bigger
from the same coarse data. The format is 2 bytes/row (a single byte
can only address 8 columns) -- byte 0 is the left 8 columns, byte 1
the right 8, both MSB-first; `font.h`/`font.c`/`vga.c`'s
`vga_draw_char()` all updated to match. `FONT_GLYPH_BYTES` (the raw
custom-font-file format `font_load()` reads) is 8192 now, was 2048.

The compiled-in fallback font (`font8x8_basic`, used until a real TTF
is loaded) has no hand-drawn 16x16 art -- redrawing 256 glyphs by hand
wasn't in scope here. It's upscaled 2x (nearest-neighbor) from the
existing 8x8 art instead: legitimately bigger, exactly as recognizable
as before, honestly blockier than real 16x16 art would be rather than
pretending otherwise. Any real .ttf loaded via `font_load_ttf()` gets
genuine 16x16 detail; the upscale only backs the builtin default and
whatever a loaded font's own coverage gaps fall back to.

### Everything else: the UI was still laid out for 8px text

Doubling the glyph size without touching layout would have meant
clipped title-bar text, an 'x' twice the size of its close button,
and dialog windows too narrow for their own labels. Fixed, all
traceable to the glyph-size bump:

- `terminal.h`'s `TERMINAL_CHAR_W/H` 8->16 -- this alone correctly
  resized every terminal-window computation in `gui.c`/`kernel.c`,
  since those already read the macro instead of a hardcoded `8`.
- `starlightos_gui.h`: `GUI_LIST_ROW_H`/new `GUI_TEXT_LINE_H` 12->22 (Files
  list rows and About/Disk/Help body text line pitch).
- `starlightos_gui.c`: `HEADER_H` (title bars *and* the top bar) 14->26; the
  close button 8x8->18x18 with its separately-hardcoded hit-test
  region updated to match; the topbar's clock/wordmark centering math
  (which multiplied by the old 8px char width) fixed to 16px; the
  dock icon tooltip bubble resized from an 11px-tall box to fit 16px
  label text; a button-label vertical-centering helper's `(h-8)/2`
  fixed to `(h-16)/2`.
- `gui.c`: every place that had its *own* separately-hardcoded copy
  of "HEADER_H + 3 = 17" (there was no single source of truth for
  that number) updated to the new 29; the About/Disk/Help window
  widths (220/200/230) roughly doubled (380/440/560) since the same
  text is now twice as wide per character; the Files window widened
  200->320 for 8.3-format filenames at the new glyph width; the Files
  list's folder-icon size and text offset enlarged to fit the taller
  rows; the Paint app's toolbar (was a fixed 10px-tall strip) resized
  to 20px with bigger/re-spaced color swatches, since a 10px bar
  can't contain 16px label text.

### How this was actually verified

Beyond the usual "everything recompiles clean" check (real, but only
proves the code is internally consistent, not that it *looks*
right): a small standalone host program was built that links
`font_ttf.c` directly (it has zero OS dependencies, by design -- see
font_ttf.h), feeds it the actual `assets/JetBrainsMonoNerdFont-Regular-ASCII.ttf`
this project bundles, and prints several rasterized glyphs as ASCII
art. 'O' comes out as an actual circle, not an ellipse (confirming
last round's squish fix still holds at the new size); 'W', 'e', 'g'
(descender) all have correct, recognizable proportions. That's real
evidence the rasterizer produces correct output -- still not a
substitute for seeing it rendered on an actual boot, but a real step
up from "it compiles."

**Not verified by an actual boot.** Please `make clean && make &&
make run` and confirm text and window layout actually look right on
screen -- particularly the Paint toolbar and dock tooltips, which
got the most re-guessed pixel math this round and are the likeliest
place for an off-by-a-few-pixels mistake to be hiding.

---

## This round: 1920x1080 HD, real GRUB boot path, TTF squish fix, wallpaper reskin

**Same disclosure as every previous round: no cross-toolchain
(`i686-elf-gcc`/`x86_64-elf-gcc`), no QEMU, no network in this
sandbox.** Nothing below was compiled or boot-tested. Where a change
could be checked another way (the boot stub's actual machine code,
below), it was -- assembled and disassembled with the sandbox's
native `as`/`objdump` and read byte-by-byte, the same discipline the
x86_64 port's own README already documents. Everything else is a
careful read, not a run. Please `make clean && make && make run` (and
`make iso64 && make run_hd64` for the new x86_64 boot path) and report
back what's actually on screen.

### "The x64 stays on booting from hard disk" -- root cause found and fixed

`disk.img` (the FAT16 file-storage disk `make_disk.py` builds) has
always ended in the `0x55AA` BIOS boot signature -- ordinary for a
FAT16 boot sector -- but the bootstrap *code* area right after it was
just zero-filled. A BIOS pointed at that disk directly (as opposed to
QEMU's `-device loader` debugging shortcut, the only way this project
has ever actually started the 64-bit kernel) does exactly what the
signature says: loads the sector, jumps into the code area, and
executes zero bytes into nothing forever. No crash, no message --
just the BIOS's "Booting from Hard Disk..." line and then silence.
That silence *is* "stays on booting from hard disk."

Two changes fix this, in `x86_64_port/`:

- **`grub.cfg`** + a new `iso64` Makefile target build `starlightos.iso`, a
  real GRUB-bootable image of `starlightos64.elf` -- the actual fix, and the
  file that should be handed to a BIOS/QEMU `-hda`/`-cdrom`/`dd`
  instead of `disk.img`. Full explanation and exact commands are in
  `x86_64_port/grub.cfg`'s comments and `x86_64_port/README.md`.
- **`make_disk.py`**'s boot sector now has a tiny hand-assembled
  real-mode stub in that previously-zeroed code area instead of
  zeros -- if `disk.img` ever gets BIOS-booted directly by mistake
  again, it prints a plain message ("this is a data disk, boot
  starlightos.iso instead") over BIOS teletype and halts, rather than
  hanging with no signal at all.

### TTF loader was squishing every glyph -- found and fixed

`font_ttf.c`'s rasterizer was scaling the X axis by `units_per_em`
but the Y axis by the font's real ascent-descent span (`hhea`) --
two *different* numbers for essentially every real font (Liberation
Sans: 2048 vs. 2288). That's a non-uniform scale: circles come out as
ellipses, every letterform comes out shorter/wider than it was
actually drawn. Fixed `to_raster()` to use the same denominator
(`units_per_em`) for both axes, so glyph proportions are preserved --
`top`/`bottom` now only control vertical placement, not the scale
factor. A very tall accented glyph can now clip at the cell edge
instead of being squashed to fit; for an 8x8-pixel bitmap target,
that's a far smaller defect than distorting every glyph's shape.

### Real 1920x1080 HD

`vga_try_enable_vbe()`'s call site in `kernel.c` now requests
1920x1080 instead of 640x480 -- the Bochs/QEMU DISPI interface this
project already talks to directly supports it, still at 8bpp indexed
color (no framebuffer format change, just a bigger one). `VGA_MAX_
WIDTH`/`HEIGHT` in `vga.h` moved from 640x480 to 1920x1080 to match,
which is also an implicit bump in how much static RAM the kernel
assumes it has: `gui.c` keeps three full-screen 8bpp buffers
(desktop/paint_canvas/frame_buffer) at that ceiling, ~2 MiB each,
~6.2 MiB of `.bss` total. The mode-13h-first-then-upgrade fallback
is untouched, so a VBE-less target still safely lands on 320x200
exactly as before.

### Desktop reskin -- "dusk over the mountain" wallpaper

`build_desktop()` in `gui.c` went from a flat single-hue vertical
gradient to an actual scene, aiming at the same read as the reference
screenshot (dusk sky, mountain silhouette, sakura) using only flat-
fill/dither primitives -- there's no image decoder in a freestanding
kernel, so nothing here is a decoded photo:

- A dithered 8-stop sky gradient (deep night-indigo to warm coral),
  smoother than a hard-banded fill at 256 colors.
- A soft three-ring sun/glow disc.
- A two-layer Fuji-style mountain silhouette (hazy far ridge, darker
  near peak) with a ragged (not razor-straight) snowcap.
- Scattered cherry-blossom dot clusters across the foreground and a
  light sprinkle over the sky, via a cheap deterministic hash
  (`scatter_hash()`) standing in for a PRNG -- there's no `rand()` in
  a freestanding kernel either.

A new 26-color "wallpaper" ramp (palette indices 64-89, named
`COL_SKY_*`/`COL_MTN_*`/`COL_BLOSSOM_*`/`COL_SUN_*` in `starlightos_gui.h`)
backs all of this in `vga_init_default_palette()` -- the existing
0-63 UI theme colors are untouched.

Also: the dock's icon size/height (`GUI_DOCK_ICON_SIZE`/`GUI_DOCK_H`
in `starlightos_gui.h`) grew from 28px/34px to 44px/64px so it reads as a
real dock on a 1920x1080 canvas instead of looking lost in the
corner; windows now open centered on screen instead of pinned at a
fixed (40, 30); and the Terminal app's default size grew from 40x25
to 80x32 characters (`terminal.h`) with a compact 12-pixel monospaced
pitch; the backing store remains capped at 80x56.

---

# Previous rounds

## Files fix, real app SDK, GUI reskin, terminal-as-app, VBE

**Note from a later round: the "no cross-toolchain, no QEMU" caveat
below applied to this round too** -- it's not specific to the latest
one at the top of this file; every round of this project has had the
same sandbox limits.

### Files app was dead code — fixed for real this time

The FILES window in the GUI was calling `fat16_get_names()`, which
returns names only — no directory info at all — and just printed plain
text with no click handling whatsoever. That's the "folders appear as
extensionless files, and don't work" bug: every folder was visually
indistinguishable from a file and clicking one did nothing, because the
code drawing that window had no way to know which entries were
directories, let alone navigate into one.

The odd part: a **fully working** version already existed in
`apps/files.c` — real navigation, `..` handling, everything — compiled,
linked, and never called by anything. `gui.c` had its own separate, much
worse implementation instead. Rewrote the FILES window from scratch using
`fat16_get_entries()` (which *does* return real `is_dir` flags), with
actual click-to-navigate, a synthetic `..` row when not at the root,
keyboard up/down + Enter, and scrolling for directories with more than 8
entries. Deleted the orphaned `apps/files.c` version along with the rest
of `apps/` (see below).

### The old "app SDK" (`starlightos_gui.h`/`.c` + `apps/*.c`) was 100% dead too

Every function in the old `starlightos_gui.c` was a `(void)arg;` no-op, and
`apps/about.c`, `apps/disk.c`, `apps/help.c`, `apps/paint.c` had
`init/update/draw/close` hooks that were compiled, linked into the final
binary, and never invoked by anything — `gui.c` re-implemented each
window inline with raw `vga_fill_rect()` calls instead. Deleted all of
it and wrote a **real** SDK in its place: `starlightos_gui.h`/`starlightos_gui.c` now
has actual window chrome (drop shadow, header bar, single round close
button — GNOME's one-button style, no minimize/maximize clutter), a
scrollable list widget (what the Files fix above is built on), flat
vector icons drawn from primitives (folder/disk/paint/help/terminal/
file — no bitmaps, just proportional rects/lines so they scale to any
icon size), a top bar, and a bottom dock with hover tooltips. `gui.c` now
actually calls into this for every window instead of hand-drawing each
one separately, so a future app gets real chrome and widgets for free.

### GUI reskin — GNOME-ish top bar + dock, KDE-ish flat chrome

- A **top bar** (GNOME shell style) showing the current app name (or
  "ACTIVITIES" at the desktop), a centered clock, and a small wordmark.
- A **bottom dock** replacing the old fixed desktop icons — six icons
  (About/Disk/Files/Paint/Help/Terminal), hover-highlighted, with a
  little tooltip label popping up above whichever one you're pointing
  at, the way GNOME's dash does it.
- **Window chrome**: dithered drop shadow (a 1px checkerboard of two
  shades — the closest thing to a soft shadow that flat 256-color mode
  can do without real alpha blending), a dark header bar, a single round
  close control top-right.
- A small Adwaita-inspired accent palette added to the VGA DAC (indices
  48–63 — blue/charcoal/warm-gray/red/yellow), named as `COL_*` constants
  in `starlightos_gui.h` so every app pulls from the same theme instead of
  picking arbitrary palette indices.

This is still fundamentally an 8-bit palette, blocky-pixel GUI — there's
no getting real GNOME out of a 256-color linear framebuffer with no
compositor — but it should read as an intentional, cohesive flat theme
now rather than "squares and terminal text."

### Terminal is an app now, not a separate mode

The shell used to be reached by typing `gui` *from* a full-screen
terminal, and leaving the GUI meant dropping back into that same
full-screen terminal — two separate top-level modes. Now there's one
mode: the desktop. `kernel_main` boots straight into `gui_run()`, and the
shell lives inside a **Terminal** window (`APP_TERMINAL`) opened from the
dock like anything else. This didn't need a rewrite of the shell itself —
`kernel.c`'s character-grid terminal already kept a full backing store
(`screen_chars`/`screen_colors`) and a `terminal_redraw()` that repaints
it; the only change (`terminal_set_origin()`, in the new `terminal.h`)
was letting that grid draw at a movable pixel offset instead of being
pinned at `(0,0)`, so the window manager can host it instead of it
owning the whole screen. Boot log output still happens before the GUI
starts (origin `(0,0)`, exactly as before) and is sitting in that
scrollback the first time you open the Terminal app.

Keys route based on what's focused: with the Terminal open, everything
except ESC goes straight to the real shell (`shell_handle_character()`)
— including digits, which would otherwise be GUI app-switch shortcuts.
ESC closes the window back to the desktop. The `gui` shell command is now
just an informational message, since you're always on the desktop
already.

### Font: default is correct (verified, not assumed) + a real loader

**A correction, and I want to be upfront about it:** partway through this
round I "fixed" `vga_draw_char`'s bit order based on a comment already in
`font8x8.c` claiming the font was LSB-first, without actually rendering a
glyph to check. That comment was wrong, and my change would have
mirrored every asymmetric letter — the exact bug it claimed to fix. Caught
before finishing (you asked what I was talking about, which was the right
challenge), verified properly this time by hand-rendering `F`, `L`, `P`,
`7`, `b`, `d` etc. as ASCII art both ways, confirmed MSB-first
(`0x80 >> col`, the original code) is correct, reverted, and fixed the
stale comment at its source in `font8x8.c` so it stops misleading whoever
reads it next (a prior round, or me). Left a note in both places pointing
at *how* to verify it (render a glyph) rather than just asserting an
answer, since trusting an assertion instead of checking is exactly what
went wrong. Sorry for the churn — better to say this plainly than bury it.

On top of that (now-confirmed-correct) default, added an actual small
font *loader*: `font.h`/`font.c` is a tiny manager `vga_draw_char()` now
goes through, and `font_load()` can pull a raw 2048-byte 8×8 bitmap font
(256 glyphs × 8 bytes, MSB-first rows) off the FAT16 disk at runtime and
swap it in — a `font`/`font default` shell command exposes it. There's no
built-in "second font" to switch to (`font8x8`'s block/box tables are
box-drawing/block symbol sets, not alternate alphabets, so they can't
stand in as a full readable font) — this is a loader for a font *you*
provide, which is what "font loader" can actually mean here.

### Bigger screen — real VBE support, with a safe fallback

Added `pci.c`/`pci.h` (minimal config-space scanner) and `vbe.c`/`vbe.h`
(Bochs/QEMU "DISPI" register interface) to request a 640×480 linear
framebuffer directly from protected mode — no BIOS call needed, since
QEMU's default "std" VGA device implements these registers in hardware/
emulation. `kernel_main` always enters plain mode 13h first (guaranteed
to work), then tries the VBE upgrade; if the PCI probe doesn't find a
matching device, or the DISPI ID register doesn't respond as expected,
it leaves mode 13h exactly as it was and logs which one it landed on.
There's no code path where calling this can leave the display worse off
than not calling it.

`vga.c`/`vga.h` now track width/height/framebuffer pointer at runtime
instead of hardcoding 320×200, and every drawing primitive (and `gui.c`'s
layout) reads those instead of a compile-time constant, so the same code
draws correctly at either resolution. The `desktop`/`paint_canvas`
buffers are sized to a 640×480 ceiling (`VGA_MAX_WIDTH/HEIGHT`) regardless
of which mode ends up active.

The previous round was right to be cautious here ("neither of which I can
responsibly do blind in this sandbox") — I'm doing it now specifically
*because* the fallback is unconditional and can't regress the plain
320×200 path if the probe fails, not because the caution was wrong.

### Cleanup

Deleted `apps/` entirely (dead code, see above) and updated the `Makefile`
accordingly — added `pci.o`, `vbe.o`, `font.o`.

## How to boot it

```sh
qemu-system-i386 -drive format=raw,file=disk.img -kernel starlightos.bin
```

Rebuild from source with your real `i686-elf-gcc` (MacPorts) — please do
this rather than trusting the `starlightos.bin` in this archive, which was
linked with a substitute host toolchain (`gcc -m32`, no cross `-lgcc`
available in this sandbox) purely to prove the object files all resolve
against each other. It's not necessarily what a real `i686-elf-gcc` build
produces:

```sh
make clean
make
make run
```

If you ever want a fresh filesystem (reset anything you `touch`ed/
`write`/`cp`'d in the shell):

```sh
python3 make_disk.py
```

## What to check when you boot it

1. **Boot log** should print in the top-left, then the desktop should
   appear on its own (no `gui` command needed).
2. **Files** (dock icon or press `3`) — click a folder to enter it, `..`
   at the top to go back up; try a directory more than 8 entries deep to
   see the scrollbar.
3. **Terminal** (dock icon or press `6`) — should show the boot log
   already in its scrollback; type a command; press ESC to close it back
   to the desktop, reopen it and confirm your shell state (cwd, etc.)
   was preserved.
4. **Text should render right-side-up** — `F`, `L`, `7` etc. should look
   like themselves, not mirrored.
5. **Boot log line** should say either "640x480 (VBE linear framebuffer)"
   or "320x200 (mode 13h, no VBE device found)" — either is fine, it's
   just telling you which one it landed on. If you're on the MacPorts
   QEMU setup with default `-vga std`, I'd expect the VBE one.
6. **`font`** (inside Terminal) with no arguments should print the active
   font name; `font default` should be a no-op if nothing else was ever
   loaded.

Let me know what's actually on screen and I'll keep going from there.

---

# Previous rounds



Previously the shell rendered into the hardware's own 80×25 text mode
(`0xB8000`) and the GUI lived in a completely separate 320×200×256
graphics mode, switching VGA registers back and forth each way. **Now
everything renders into the same 320×200×256 framebuffer, permanently,
from the moment the kernel boots.** `kernel_main` calls
`vga_enter_mode13h()` once, right at the start, and nothing ever switches
back.

The shell keeps a full backing store of its own screen (`screen_chars`/
`screen_colors`, 40×25 characters, in `kernel.c`) and draws each character
as an 8×8 glyph via `vga.c`'s primitives, with a visible cursor (a small
underline, redrawn as it moves — real hardware text cursors don't exist
in graphics mode, so this is the graphics-mode equivalent). Opening the
GUI just means the desktop starts drawing over that same framebuffer;
closing it calls a new `terminal_redraw()` that repaints the shell's
screen from its backing store, so you land right back where you were —
no mode switch, no flicker, no separate "text mode" to fall out of sync
with.

**The trade-off, named honestly:** at 8×8 pixels per character this
terminal is 40 columns wide instead of 80, so more output wraps than it
used to (the `help` listing, `ls -l` with long filenames, etc.). If that
turns out to feel more cramped than unified-and-cohesive, it's a one-line
revert (`GFX_COLS`/`GFX_ROWS` back to a real 80×25 `0xB8000` terminal) —
just say so once you've actually seen it.

## Bugs fixed from the previous round

Going through what you'd built on top of my last pass turned up a cluster
of real bugs, all now fixed:

- **The GUI never restored text mode** (`vga_exit_mode13h()` had been
  removed) and a half-finished parallel "graphics terminal" fallback
  (`terminal_gfx.c` + a `graphics_terminal` flag in `kernel.c`) was
  broken and unreachable — `terminal_gfx.c` called a function
  (`font8x8_get`) that didn't exist anywhere, and wasn't even wired into
  the `Makefile`. **Deleted** rather than patched, since the unified
  single-framebuffer design above replaces what it was reaching for
  anyway, correctly this time.
- **Mirrored text.** Someone swapped my original hand-drawn font for the
  real public-domain `font8x8` library (`font8x8_basic.h` etc — good
  swap, much nicer font) but that library stores each row **LSB-first**
  (bit 0 = leftmost pixel), while `vga_draw_char` was still reading
  **MSB-first**. Every glyph rendered as a horizontal mirror image. Fixed
  the bit order and the character spacing (this font is a full 8px wide,
  not the 5px my old one was, so the old cursor-advance-by-6 was
  overlapping letters).
- **Choppy paint strokes — likely your main "choppy" complaint.** Paint
  was plotting a dot at the exact mouse position on every PS/2 packet,
  with no interpolation. Drag the mouse quickly and the packets arrive
  father apart than a brush-width, leaving visible gaps — a classic
  paint-app bug. Fixed with a proper Bresenham line (`paint_stroke()` in
  `gui.c`) connecting each new packet's position back to the last one, so
  fast drags now draw a continuous line instead of a dotted one.
- **The DISK window was rescanning the entire FAT table every single
  frame.** The previous round's GUI loop had been changed to
  unconditionally redraw every iteration (rather than only when
  something actually changed), and `fat16_free_bytes()` walks every FAT
  entry doing a disk read each time — with the DISK window open, that's
  a full FAT scan roughly 100+ times a second, which is a very plausible
  source of general GUI sluggishness beyond just paint. Restored the
  dirty-flag-based redraw (only redraw on mouse movement/clicks, a
  window/app change, or once a second for the clock) and cache the disk
  stats, refreshing them only when the DISK window is freshly opened or
  once a second while it stays open.
- **The disk window itself had been stubbed out** ("FAT16 DISABLED" /
  "MOUSE TEST" placeholder text) — restored real free/used/total bytes.
- **A genuinely correct fix from that round, kept as-is:** the `gui`
  command sets a `gui_requested` flag instead of calling `gui_run()`
  directly. That matters — the shell's command dispatch runs from inside
  the keyboard IRQ handler, *before* its EOI is sent to the PIC. Entering
  a blocking loop there leaves IRQ1 "in service" for as long as the GUI
  is open, which blocks lower-priority cascaded IRQs — including the
  mouse's IRQ12 — from ever firing. Checking the flag from the idle loop
  instead, well outside any interrupt context, avoids that entirely.
  Whoever tracked that down was right; I kept the mechanism (and it's
  what makes the new "repaint on return" design work cleanly too).

## New GUI features

- **Draggable windows.** Click-drag anywhere on a window's title bar
  (not the ✕) to move it around; it's clamped to stay on screen.
- **Icon hover highlighting** — icons brighten when the mouse is over
  them, redrawn fresh each frame rather than baked into the static
  desktop.
- **Taskbar shows the current app** ("STARLIGHTOS - PAINT", etc.) instead of
  always just "STARLIGHTOS".

## New non-GUI features (you specifically asked for "not just GUI")

- **`mv OLD NEW`** — rename a file in place (same cluster chain, just the
  directory entry's name changes; `fat16_rename()` in `fat16.c`).
- **`cp SRC DST`** — copy a file (up to 4 KiB; `fat16_read_all()` +
  `fat16_write()`).
- **Tab completion.** Start typing a filename to any command and press
  Tab — if it unambiguously matches exactly one file in the root
  directory, it completes in place. Multiple/zero matches do nothing
  (no disruptive listing, just silence).
- **`irqdebug`** (added in the round I built on top of) shows keyboard/
  mouse IRQ counters — now documented in `help`.

## Screen size / resolution

I didn't touch the actual VGA resolution (320×200 is the real hardware
ceiling for the simple linear framebuffer mode this project uses — going
higher means either a much more complex planar "Mode X" or genuine VBE
support, neither of which I can respons­ibly do blind in this sandbox).
If the QEMU *window* feels tiny on a Retina display, that's a QEMU
display setting, not something StarlightOS itself controls:

```sh
qemu-system-i386 -display cocoa -drive format=raw,file=disk.img -kernel starlightos.bin
```

MacPorts' QEMU cocoa backend lets you drag the window's corner to resize
it — it'll scale the 320×200 image up (blocky/pixelated, since it's
nearest-neighbor, not blurry-smooth) rather than changing the OS's actual
resolution. That's almost certainly the "bigger window" fix you want.

## How to boot it

Prebuilt `starlightos.bin` + `disk.img` are in this archive.

```sh
qemu-system-i386 -drive format=raw,file=disk.img -kernel starlightos.bin
```

To rebuild from source with your existing `i686-elf-gcc` (MacPorts):

```sh
make clean
make
make run
```

If you ever want a fresh filesystem (reset anything you `touch`ed/
`write`/`cp`'d in the shell):

```sh
python3 make_disk.py
```

## What to check when you boot it

1. **Boot log** should now render in the 40-column graphics terminal
   (crisp, correctly-oriented text) instead of the hardware's 80-column
   text mode.
2. **`gui`** → desktop should look right-side-up (font mirror fix);
   dragging a window by its title bar should move it; hovering an icon
   should highlight it.
3. **Paint** → drag quickly with the left button held; strokes should be
   continuous lines now, not dotted.
4. **DISK window** → should show real numbers immediately and stay
   responsive (no more per-frame FAT rescans).
5. **ESC/Q from the GUI** → should land you back in a normal, working
   shell prompt, not a frozen or blank screen.

Let me know what's still off and I'll keep iterating.
