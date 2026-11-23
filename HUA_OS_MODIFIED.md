## Phase 1 — Kernel foundations

- [x] UEFI boot, console, keyboard
  → Limine boot protocol and framebuffer terminal
- [x] Physical memory manager (PMM)
  → Linux and FreeBSD memory-management concepts
- [x] Virtual memory manager (VMM)
  → Linux page-table handling and FreeBSD address-space design
- [x] Kernel heap allocator
  → Solaris/illumos slab allocator, Linux SLUB, FreeBSD UMA
- [x] ACPI and timer infrastructure
  → Linux hrtimers and FreeBSD callouts
- [x] Threads, scheduler, wait queues and synchronization primitives
  → Linux, FreeBSD and Windows scheduling concepts
- [x] GDT, IDT, interrupt dispatch and CPU exception handling
- [x] Page-fault handling and kernel/user exception separation
- [x] QEMU UEFI build and boot workflow
- [x] In-kernel subsystem self-tests
- [ ] Serial logging, richer panic diagnostics and symbolized stack traces

## Phase 2 — Processes and user mode

- [x] Process lifecycle, process control structures and address spaces
  → Linux task management and Fuchsia/Zircon process objects
- [x] User/kernel separation and user-memory validation
  → Linux copy_from_user-style protection
- [x] System-call interface and SYSCALL/SYSRET path
  → Linux-style stable syscall ABI
- [x] ELF64 loading and Ring 3 execution
  → Linux binfmt_elf and FreeBSD imgact_elf
- [x] User-process exception isolation, termination and reaping
- [x] Resource cleanup and failure-recovery testing
- [ ] Demand paging and copy-on-write
  → Linux and FreeBSD virtual memory
- [ ] IPC: pipes, signals, shared memory and message passing
  → QNX, seL4, Fuchsia and Linux
- [ ] Futexes
  → Linux futex, Fuchsia zx_futex and Windows WaitOnAddress
- [x] NX, W^X and page-level memory permission enforcement
  → OpenBSD and Linux security concepts
- [ ] SMEP/SMAP, stack canaries and additional kernel hardening
  → OpenBSD and Linux
- [ ] First boot and validation on real hardware

## Phase 3 — SMP bring-up
- [ ] AP startup, per-CPU data
  → Linux (smpboot, per-CPU variables)
- [ ] Cross-CPU locking, IPIs, TLB shootdown
  → Linux (queued spinlocks, RCU), FreeBSD (SMPng fine-grained locking), DragonFly BSD (per-CPU data, lockless designs)
- [ ] SMP-safe PMM/VMM/heap
  → Linux (per-CPU page lists), Solaris (magazine layer in the slab allocator)
- [ ] Multi-CPU scheduling
  → Linux (per-CPU runqueues, scheduling domains), FreeBSD ULE

## Phase 4 — Device foundation
- [ ] PCI/PCIe enumeration
  → Linux (PCI core), FreeBSD
- [ ] IOAPIC, MSI/MSI-X routing
  → Linux (generic IRQ layer, IRQ domains)
- [ ] Driver model
  → FreeBSD (newbus: smallest to understand), NetBSD (autoconf), Linux (bus/device/driver + sysfs), macOS (IOKit: matching and object-oriented drivers), Fuchsia and MINIX 3 (user-space, restartable drivers)
- [ ] ACPI AML interpreter
  → ACPICA (reference implementation), uACPI (small, built for hobby kernels)

## Phase 5 — Storage and filesystem
- [ ] Block layer, GPT
  → Linux (blk-mq: multi-queue, built for NVMe parallelism)
- [ ] virtio-blk, NVMe, AHCI
  → virtio spec (cleanest to start), FreeBSD nvme(4) (compact), Linux
- [ ] VFS, fd tables, mounts
  → BSD vnode/VFS (from SunOS, clean layering), Linux (dentry/inode caches), Plan 9 (per-process namespaces, 9P)
- [ ] initramfs, tmpfs, devfs
  → Linux (initramfs, tmpfs), FreeBSD (devfs)
- [ ] First on-disk FS, page/buffer cache
  → ext2 (simple, well documented), FAT32 (needed anyway for the EFI System Partition)
  → Long-term: ZFS (checksums, snapshots), APFS (clones/snapshots), ext4 (journaling via jbd2)
  → Cache: Linux (unified page cache, readahead, writeback), FreeBSD (VM and buffer cache unified)

## Phase 6 — Userland and OS API
- [ ] POSIX vs custom API
  → POSIX: Linux/BSD (existing software ports easily)
  → Custom: Fuchsia (handles), Redox (schemes), Haiku and SerenityOS (native API on top of POSIX-like base)
- [ ] Toolchain, libc, dynamic linker
  → musl (small, readable, easy to port), mlibc (designed for hobby OSes), FreeBSD rtld and musl's ld.so (simple dynamic linkers)
- [ ] TTY/PTY
  → Linux tty layer, BSD pty
- [ ] Init, shell, utilities
  → Init: runit/s6 (minimal), systemd (dependencies, cgroups), launchd (macOS)
  → Utilities: BusyBox/toybox (compact), FreeBSD and SerenityOS (cohesive base system in one repo)

## Phase 7 — Input and USB
- [ ] xHCI, USB HID, USB storage
  → Linux (broadest, best-tested USB stack), FreeBSD
- [ ] i8042, I2C-HID (laptop touchpad)
  → Linux (i2c-hid, hid-multitouch), Windows (Precision Touchpad protocol)
- [ ] Unified input events
  → Linux evdev + libinput (gestures, palm rejection, acceleration in user space)

## Phase 8 — Graphics and HUA Desktop (user-space)
- [ ] Framebuffer exposed to user space, double buffering
  → Linux DRM/KMS (dumb buffers, atomic modesetting)
- [ ] 2D rendering, fonts
  → Skia (Android/Chrome), Cairo, SerenityOS LibGfx (small)
  → Fonts: bitmap/PSF first, then FreeType + HarfBuzz
- [ ] Compositor/window server, client protocol
  → Wayland (clients draw into shared buffers, tiny protocol), macOS Quartz (smoothness), Windows DWM
  → Readable at hobby scale: SerenityOS WindowServer, Haiku app_server, Plan 9 rio
- [ ] Window manager, focus, cursor
  → Haiku (consistent behavior), macOS and Windows (UX polish)
- [ ] Toolkit, taskbar, launcher
  → Toolkit: Haiku Interface Kit, SerenityOS LibGUI, Qt (layouts, signals/slots)
  → Taskbar: Windows, KDE Plasma. Launcher: macOS Spotlight
- [ ] Terminal, file manager, settings
  → SerenityOS (all three are small enough to read), KDE Dolphin, macOS Finder

## Phase 9 — Networking
- [ ] Stack: Ethernet, ARP, IPv4, ICMP, UDP, TCP
  → FreeBSD (reference-quality BSD stack, mbufs), Linux (sk_buff, congestion control like CUBIC and BBR)
  → Small and readable: lwIP, smoltcp (Rust)
- [ ] NIC drivers
  → virtio-net and e1000 (public specs), USB Ethernet (CDC-ECM/NCM)
- [ ] Socket API
  → BSD sockets (the model everyone copies, including Winsock)
- [ ] DHCP, DNS, loopback
  → Run DHCP/DNS as user-space daemons (dhcpcd, unbound)
- [ ] WiFi
  → Linux (iwlwifi + mac80211/cfg80211), OpenBSD (net80211, readable); expect firmware blobs
- [ ] Firewall (later)
  → OpenBSD pf

## Phase 10 — Power, performance, maturity
- [ ] Power management
  → macOS (best battery life on its own hardware), Windows (Modern Standby on x86 laptops), Linux (runtime PM, cpuidle/cpufreq, s2idle)
- [ ] Performance tuning
  → Linux (perf, ftrace, eBPF), DTrace (Solaris, FreeBSD)
- [ ] Stress/stability testing
  → Linux (syzkaller fuzzer, KASAN/KCSAN, kselftest), FreeBSD (INVARIANTS + WITNESS debug options)
- [ ] Security hardening
  → OpenBSD (pledge/unveil, KARL, audit culture), seL4 (formally verified), Fuchsia (capability sandboxing), Linux (seccomp, namespaces, LSMs)

## Continuous tracks
- Dev culture and docs: SerenityOS (CI, tests, readable code), Fuchsia docs, OSDev wiki
- Books: xv6 book, OSTEP, Windows Internals, The Design and Implementation of the FreeBSD OS, Modern Operating Systems
