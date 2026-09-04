# Titan Kernel v3 — "Glass Edition"

A from-scratch, single-file, protected-mode x86 kernel with its own graphical desktop environment. No Linux, no BSD, no existing kernel underneath — this boots directly on bare metal (or a VM) via GRUB/Multiboot and takes over the machine from the first instruction.

## What it actually is

Titan is a **hobby-scale OS kernel**, written in C with a bit of inline assembly, that:

- Boots via the **Multiboot 1** specification (GRUB-compatible), requesting a linear framebuffer directly from the bootloader.
- Sets up its own **GDT, IDT, and PIC remapping** from scratch — no borrowed boot code.
- Drives its own **PS/2 keyboard and mouse drivers**, **PIT timer**, and **CMOS RTC**.
- Renders a full **windowed desktop GUI** entirely by drawing into an off-screen backbuffer and presenting it to video memory — no external graphics library, no font library (there's a hand-rolled 7-row bitmap font).
- Implements its own **in-memory hierarchical filesystem** with directories, file ops (`cp`, `mv`, `rmdir`, `wc`, `head`, `tree`, `ll`, …), and a working **File Manager** app.
- Has a **persistent encrypted disk volume** — ATA PIO driver underneath, AES-128-CTR encryption keyed from the login password + salt, CRC32 integrity checking, fail-closed on any mismatch.
- Ships a small suite of built-in apps: **Notepad, Calculator, Paint, Clipboard (with history), Clock view, Task Manager, App Launcher, Toast notifications, Lock screen** with real login/password flow.
- Includes a **real network stack**: Ethernet, ARP, IPv4, ICMPv4 (ping), a PCI bus scanner, and an **Intel e1000 NIC driver** written against real hardware/QEMU register semantics — plus shell commands (`ifconfig`, `ping`, `arp`, `lspci`).
- Has its own **terminal/shell** with command history (↑/↓), tab-completion, scrollback, and ~30 built-in commands.

## Why "Glass Edition" (v3)

The changelog at the top of `titan.c` is refreshingly honest about what changed and why:

- **v2's problem:** every shape was drawn straight to video memory, one bounds-checked pixel call at a time. On a large framebuffer, a full redraw was on the order of a million checked calls — and it fired on *every mouse packet*, which is why the cursor felt laggy.
- **v3's fix:** everything draws into an off-screen backbuffer using tight `rep stosl`/`rep movsl` loops, and the finished frame is presented to video memory once per redraw. Different order of magnitude, and it also killed the flicker/tearing.
- **Visual refresh:** rounded-corner translucent "glass" windows that blend with the desktop gradient, layered drop shadows, a glowing taskbar, hover/press button glow, a blinking terminal cursor, and a smooth gradient desktop instead of flat colour bands.

The header comment is also upfront about limits: **no paging, no real filesystem-on-disk beyond the encrypted blob, no multitasking.** It's not claiming to be production-grade — it's a real, QEMU-tested kernel that got meaningfully faster and better-looking in this revision.

## Source layout

```
titan.c                 Main kernel: boot, GDT/IDT/PIC, drivers, GUI, shell, FS  (~4,300 lines)
ossystemsafety.{c,h}    Small fail-closed safety layer (bounds checks, safe wipes)
persist_storage.{c,h}   ATA PIO disk driver + AES-128-CTR encrypted volume + CRC32
lockscreen.{c,h}        Login / lock screen with password handling
timeanddate.{c,h}       RTC-backed clock/date formatting
calculator.{c,h}        Integer-only 4-function calculator app
paint.{c,h}             Fixed-canvas bounds-checked drawing pad
cliphist.{c,h}          Clipboard + 5-entry clipboard history
applauncher.{c,h}       App launcher (Ctrl+Alt+A)
toast.{c,h}             Transient notification popups
taskmgr.{c,h}           Window/task list
clockview.{c,h}         Full-screen clock/date view
filesystem.h            In-memory hierarchical FS types (File Manager)
network/                Full stack: PCI, e1000 NIC, Ethernet, ARP, IPv4, ICMP
linker.ld, build.sh     Linker script + build/ISO pipeline (GCC + GRUB rescue image)
titan.bin               Built kernel binary
```

## Build

```bash
./build.sh
```

Compiles everything with `gcc -m32 -ffreestanding -fno-stack-protector -fno-pic -nostdlib`, links with a custom `linker.ld`, and packages the result into a bootable `titan.iso` via `grub-mkrescue` — ready to boot in QEMU or a VM.


