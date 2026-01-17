# DeltaOS

A modern operating system kernel written in C and assembly for x86-64 architecture, featuring a UEFI bootloader, preemptive multitasking, virtual memory management, and a graphical window manager (WIP).

## Project Structure

### Core Components

- **[bootloader/](bootloader/)** - UEFI bootloader (`DeltaBoot`)
  - UEFI protocol implementation
  - ELF kernel loader
  - Graphics and console drivers
  - Configuration and menu system

- **[kernel/](kernel/)** - Operating system kernel
  - x86-64 architecture-specific code (MMU, interrupts, CPU, context switching)
  - Process and thread management with scheduler
  - Memory management (PMM, VMM, kernel heap)
  - Filesystem support (DeltaArchive, initrd, tmpfs)
  - Device drivers (keyboard, mouse, PCI, serial, RTC, framebuffer)
  - IPC (Inter-Process Communication) channels
  - Interrupt and exception handling

- **[user/](user/)** - User-space components
  - libc implementation
  - Dynamic linker (`ld`)
  - Initial system binaries (init, window manager)

- **[tools/](tools/)** - Build utilities
  - `darc` - DeltaArchive creation tool for packaging initrd

- **[docs/](docs/)** - Documentation
  - Architecture specifications
  - Boot protocol
  - Filesystem formats
  - Syscall documentation

## Building

### Prerequisites

- GCC/Clang with x86-64 support
- GNU Make
- Binutils
- NASM (for assembly)
- QEMU (for running)
- mtools (for disk image creation)
- sgdisk (for partition management)
- OVMF firmware (optional, for UEFI emulation)

### Build Commands

```bash
# Build all components
make all

# Build specific components
make kernel
make bootloader
make user
make tools

# Clean build artifacts
make clean

# Build and run in QEMU
make run
```

## Running

Execute `make run` to:
1. Build all components
2. Create a 64MB GPT disk image
3. Format as FAT32 with EFI partition
4. Install bootloader and kernel
5. Launch QEMU with the disk image

The system will boot via UEFI, display the bootloader menu, load the kernel, and start user-space processes.

## Architecture

### Bootloader Flow
1. UEFI firmware loads `BOOTX64.EFI`
2. Bootloader initializes graphics and console
3. Displays menu and configuration options
4. Loads and verifies kernel ELF image
5. Sets up paging and transfers control to kernel

### Kernel Features
- **CPU**: Multi-core support with per-CPU state management
- **Memory**: Paging-based virtual memory, buddy allocator for physical memory
- **Processes**: Preemptive multitasking with priority-based scheduling
- **Devices**: Unified driver model with protocol interfaces
- **Filesystem**: Support for multiple filesystem types

### System Boot Sequence
1. Kernel initializes CPU state and MMU
2. Sets up exception handlers and interrupt controller
3. Initializes memory management (PMM, VMM)
4. Mounts filesystems and loads initrd
5. Spawns init process from initrd
6. Init process launches system daemons and window manager

## Documentation

Full documentation is available in the [docs/](docs/) folder and can be built with MkDocs:

```bash
mkdocs serve
```

Topics covered:
- Boot protocol and UEFI integration
- Executable format (ELF64)
- Archive format for initrd
- Filesystem specifications
- System call interface

## License

GNU General Public License v3