#!/bin/bash
set -e
CC=gcc
CFLAGS="-m32 -ffreestanding -fno-stack-protector -fno-pic -O2 -Wall -Wextra -c -Inetwork"
LDFLAGS="-m elf_i386 -T linker.ld -nostdlib"

$CC $CFLAGS titan.c -o titan.o
$CC $CFLAGS timeanddate.c -o timeanddate.o
$CC $CFLAGS lockscreen.c -o lockscreen.o
$CC $CFLAGS ossystemsafety.c -o ossystemsafety.o
$CC $CFLAGS persist_storage.c -o persist_storage.o
$CC $CFLAGS calculator.c -o calculator.o
$CC $CFLAGS paint.c -o paint.o
$CC $CFLAGS cliphist.c -o cliphist.o
$CC $CFLAGS applauncher.c -o applauncher.o
$CC $CFLAGS toast.c -o toast.o
$CC $CFLAGS taskmgr.c -o taskmgr.o
$CC $CFLAGS clockview.c -o clockview.o

$CC $CFLAGS network/net_types.c -o net_types.o
$CC $CFLAGS network/pci.c       -o pci.o
$CC $CFLAGS network/ethernet.c  -o ethernet.o
$CC $CFLAGS network/arp.c       -o arp.o
$CC $CFLAGS network/ipv4.c      -o ipv4.o
$CC $CFLAGS network/icmp.c      -o icmp.o
$CC $CFLAGS network/e1000.c     -o e1000.o
$CC $CFLAGS network/net.c       -o net.o

ld $LDFLAGS -o titan.bin \
  titan.o timeanddate.o lockscreen.o ossystemsafety.o persist_storage.o \
  calculator.o paint.o cliphist.o applauncher.o toast.o taskmgr.o clockview.o \
  net_types.o pci.o ethernet.o arp.o ipv4.o icmp.o e1000.o net.o

rm -rf isodir; mkdir -p isodir/boot/grub
cp titan.bin isodir/boot/titan.bin
cat > isodir/boot/grub/grub.cfg << 'GCFG'
set timeout=0
set default=0
menuentry "Titan Kernel v3 (Graphical)" {
    multiboot /boot/titan.bin
    boot
}
GCFG
grub-mkrescue -o titan.iso isodir
echo "OK"
