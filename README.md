# DeltaOS

A hobby desktop-oriented operating system for AMD64, written in C and x86 assembly with a UEFI bootloader and a monolithic object-based capability-oriented (Currently it's basic but will be expanded) kernel

## What it does

- Boots via UEFI with a custom bootloader
- Preemptive multitasking with a RR scheduler
- Virtual memory with paging
- Dynamically linked userspace
- Shell with basic commands (`ls`, `cat`, `echo`, `cd`, `pwd`)
- Framebuffer graphics and a basic window manager (WIP)

## Building

You need:
- GCC (x86-64 cross-compiler or native)
- NASM
- GNU Make (BSD make WILL NOT work)
- mtools
- QEMU + OVMF (for running)

```bash
make        # build everything
make img    # create disk image
make run    #run in QEMU
```

## Status

It boots, runs programs,has some drivers and has a working shell. 
The window manager exists but is incomplete.

# Goals (Roughly in order but not):
- Add SMP
- Create a on-disk filesystem
- A basic true GPU driver
- USB support
- Flesh out userspace libc fully
- Finish the coreutils
- Expand the Init system
- Flesh out the capability system
- TCP/IP stack
- AArch64 support (Later down the line)

Not promises just where we're probably headed



## License

GPLv3