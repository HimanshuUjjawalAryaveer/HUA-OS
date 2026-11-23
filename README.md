# HUA OS

HUA OS is an experimental, educational operating system for the
x86_64 architecture. It is written primarily in C, with NASM
assembly for architecture-specific operations.

The project explores operating-system design by studying established
ideas from Linux, BSD, Windows, and other systems, then adapting
them to HUA OS's requirements.

HUA OS currently boots using UEFI, the Limine boot protocol, and
QEMU. It is a work in progress and is not intended for everyday use.

## Current features

### Boot and terminal
- x86_64 UEFI boot using Limine
- Framebuffer-based text console
- PSF bitmap font support
- PS/2 keyboard input
- Interactive kernel shell

### Memory management
- Physical memory manager (PMM)
- Virtual memory manager (VMM)
- Four-level x86_64 paging
- Kernel heap allocator
- Address-space management
- Page permissions and NX support
- User-memory validation and safe copy operations

### Architecture and interrupts
- GDT and IDT
- x86_64 interrupt and exception handling
- System-call entry and return support
- ACPI support
- HPET and TSC clock sources
- Timer and interrupt infrastructure

### Processes and scheduling
- Process and thread lifecycle management
- Thread scheduling and run queues
- Wait and sleep queues
- Synchronization primitives
- Independent process address spaces
- Ring 3 user-mode execution
- ELF64 loading
- System-call interface
- User-process exception handling and cleanup

### Testing
- In-kernel subsystem self-tests
- Memory-management tests
- Scheduler and synchronization tests
- Process and user-mode tests
- ELF loader and system-call tests
- Failure-injection and rollback tests

## Development environment

The primary development environment is Ubuntu under WSL2.

The project uses:
- GCC and GNU Make
- NASM
- Git
- xorriso
- QEMU
- Limine
- EDK2 OVMF firmware

On Ubuntu, the basic development packages can be installed with:

```bash
sudo apt update
sudo apt install build-essential git curl nasm xorriso qemu-system-x86

## Licensing

HUA-authored code is licensed under the MIT License; see [LICENSE](LICENSE).

The original Limine C Template license notice is preserved in
[LICENSE-LIMINE-C-TEMPLATE](LICENSE-LIMINE-C-TEMPLATE).

Third-party components remain subject to their respective licenses.
See the relevant files under `third_party/`.