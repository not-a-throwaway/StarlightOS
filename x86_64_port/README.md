# x86_64 port

Status: the x86_64 BIOS disk image boots in QEMU to the 1920x1080
desktop. `make run` uses this path; `make run32` remains available for
the 32-bit Multiboot build. Early paging identity-maps 4 GiB for the
QEMU VBE framebuffer, enables SSE state, and uses a SysV-aligned stack.
Verified locally with `make build32 build64` and QEMU 11.1.1 booting
`disk.img`; the guest reached the desktop, launched `MINES.ELF` at CPL 3,
accepted game input through `int 0x80`, and returned to the shell on exit.

The user process has a bounded ELF64 loader and a dedicated 2 MiB virtual
region backed by per-page user mappings. Kernel pages remain supervisor
only; ELF text is read-only, and data/stack pages are non-executable. A
TSS supplies a separate kernel stack for ring transitions. Syscalls expose
bounded terminal output, one-character keyboard input, and process exit.
This is a single foreground process model without a scheduler or
concurrent processes. The Minesweeper model is linked only into the user
ELF, not into the kernel.

## What's here

- `boot64.s` -- the real x86_64 boot entry point. Takes a CPU
  from 32-bit protected mode into 64-bit long mode (CPUID
  checks, PAE page tables identity-mapping 4 GiB, EFER.LME/NXE, the far
  jump into a 64-bit GDT segment), then calls straight into the same
  `kernel_main()` the 32-bit build uses -- multiboot's magic/info
  registers are saved before CPUID clobbers them, and handed to
  `kernel_main` per the SysV x86-64 calling convention (registers,
  not the 32-bit cdecl stack-push convention `boot.s` uses).
- `linker64.ld` -- links the whole kernel as `elf64-x86-64`, loaded
  at 1 MiB.
- `../idt64.s` -- 64-bit ISR/IRQ entry stubs (no `pusha`/`popa` --
  removed in long mode -- and `iretq` instead of `iret`).
- `../process.c` + `../process64.s` -- bounded static ELF64 loader,
  supervisor/user page permissions, TSS kernel stack, `int 0x80`
  syscalls, synchronous process exit, and fault return to the shell.
- `../idt.c` -- now supports both the 32-bit (8-byte) and 64-bit
  (16-byte) IDT gate formats via `#if defined(__x86_64__)`, one file
  for both targets.
- Every other `.c` file in the kernel compiles clean under
  `-m64 -mno-red-zone` with zero warnings -- same source, same file,
  both architectures.

## Earlier static verification notes

The notes below document the earlier pre-boot milestone and are retained
as historical debugging context. The current BIOS image has since been
booted in QEMU, as described above.

Same discipline as the original milestone: assembled/linked with the
sandbox's *native* x86_64 `as`/`ld` (a real match for this target,
not a substitute), then actually read the disassembly rather than
trusting a clean build. That caught real, concrete issues, not
hypothetical ones:

- `vga.c` and `shell.c` both had inline EFLAGS save/restore using
  explicit 32-bit-only instruction suffixes (`pushfl`/`popl`/`pushl`/
  `popfl`). Those aren't valid opcodes at all once actually
  targeting x86-64 -- compiling with `-m64` failed with real
  assembler errors, not warnings. Fixed by dropping the explicit
  suffix (`pushf`/`pop`/`push`/`popf`) and using `unsigned long`
  instead of `uint32_t` for the register-width variable, so the
  operand size follows the target automatically on both.
- Two `-Wint-to-pointer-cast` warnings (the multiboot info pointer in
  `kernel.c`, the VBE framebuffer address in `vga.c`) -- both real
  32-bit-to-pointer truncation-looking casts, fixed by routing
  through `uintptr_t` explicitly.
- The far jump and every absolute-memory-operand instruction in the
  32-bit portion of `boot64.s` were re-verified after every edit by
  force-disassembling the raw bytes as genuine i386 code (`objdump -m
  i386` against the extracted `.text` bytes), not just trusting the
  ELF64 container's default (and, for code that runs before the mode
  switch, actively misleading) 64-bit disassembly.
- `idt64.s`'s register save area size (15 registers x 8 bytes = 120)
  was checked against the exact `lea 120(%rsp), %rdi` offset it
  hands to `isr_handler`/`irq_handler`, in the actual linked binary,
  not just the standalone object file.
- The full kernel link was checked for zero undefined symbols with
  `nm`, and the multiboot header's magic/flags/checksum were
  independently recomputed and diffed against the linked binary's
  actual bytes.

## Boot-from-hard-disk fix (this round)

**Root cause of "stays on booting from hard disk":** `disk.img` (the
FAT16 data disk `make_disk.py` builds) has always carried a valid
`0x55AA` BIOS boot signature -- because that's just part of a normal
FAT16 boot sector -- but its actual bootstrap *code* area was zero-
filled. Point a real BIOS at that disk directly and it does exactly
what the signature tells it to: loads the sector, jumps into the
code area, and executes zero bytes into nothing, forever. No error,
no message -- just the BIOS's own "Booting from Hard Disk..." line
and then silence. That silence is the bug.

Two fixes landed this round:

1. **`make_disk.py`** now puts a tiny hand-assembled real-mode stub
   in that previously-zeroed code area (verified by disassembling
   the actual linked bytes with `as`/`objdump`, not just trusting
   the assembly source) that prints a plain-text explanation over
   BIOS teletype and halts, instead of hanging with zero signal. If
   this disk ever gets BIOS-booted directly by mistake again, you'll
   see a message, not a freeze.
2. **The real fix**: `grub.cfg` (this directory, also copied to
   `isodir/boot/grub/`) describes `starlightos.iso`, a GRUB-bootable
   image of `starlightos64.elf`. *(Update: the `iso64` Makefile target
   that used to build it has since been removed, and `run_hd64` is now
   just an alias of `run64`. The `grub-mkrescue` commands are in the
   header of `grub.cfg`.)* Today `disk.img` boots by itself: the
   `bootsector.bin` + `stage2.bin` BIOS loader that `make_disk.py` writes
   into it loads `starlightos64.bin` from the disk, which is why
   `make run` works without GRUB or the QEMU `-device loader` shortcut.

**Not verified by an actual boot** -- same caveat as everything else
in this file: no `grub-mkrescue`/`xorriso`, no QEMU, no network in
the sandbox this was written in. `grub-mkrescue` is a standard tool
doing exactly its documented job here, not something improvised, but
please actually run `make iso64 && make run_hd64` and confirm.

## What's genuinely still not done

  static verification, not a substitute for watching a CPU actually
  execute it.
- No TSS / no Interrupt Stack Table. A critical exception (double
  fault, etc.) that fires with an already-corrupt stack will
  double-fault again rather than recovering onto a known-good stack.
  The 32-bit build has this same limitation today -- not a new gap,
  just worth knowing before treating either build as hardened.
- `gdt.c`/`gdt.s`/`idt.s` (the 32-bit originals) are untouched --
  `boot64.s` and `idt64.s` are the x86_64-only counterparts, not
  replacements. The 64-bit build doesn't call `gdt_initialize()` at
  all; it just keeps using the flat GDT64 `boot64.s` already loaded
  before the jump into long mode.
- The 64-bit Makefile rules use one pattern rule per C file rather
  than per-file header dependency lists the way the 32-bit rules do
  -- `make clean64` before a rebuild is the safe move after editing
  a `.h` file, not just the one `.c` file that changed.
