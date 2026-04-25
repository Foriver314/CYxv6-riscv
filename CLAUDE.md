# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is **xv6-riscv**, a re-implementation of Unix v6 for modern RISC-V 64-bit multiprocessors. It is an educational operating system used in MIT's 6.1810 course. This fork includes custom enhancements: enhanced `printf` with width/alignment/sign flags, lazy `sbrk` allocation, orphan inode recovery (`ireclaim`), and renamed kernel functions to avoid naming conflicts.

## Build System

- **Toolchain**: RISC-V 64-bit cross-compiler (auto-detected: `riscv64-unknown-elf-`, `riscv64-elf-`, `riscv64-none-elf-`, `riscv64-linux-gnu-`, etc.). Requires `qemu-system-riscv64` >= 7.2.
- **Build**: `make qemu` — builds kernel and filesystem, then runs in QEMU.
- **Debug**: `make qemu-gdb` — starts QEMU with GDB stub. Run `gdb` in another window; port is auto-generated (`make print-gdbport`).
- **Clean**: `make clean`.
- **Filesystem only**: `make fs.img`.
- **CPUs**: Override with `make qemu CPUS=1`.

## Testing

Do not perform a full user-space test unless explicitly required.

Run via `./test-xv6.py`:
- `./test-xv6.py usertests` — full user-space test suite (timeout 600s).
- `./test-xv6.py -q usertests` — quick tests only (timeout 300s).
- `./test-xv6.py crash` — crash recovery tests (log, orphaned file, orphaned directory).
- `./test-xv6.py log` — log recovery test only.
- `./test-xv6.py <regex>` — runs matching `usertests` subtest.

The script spawns `make qemu`, drives it via stdin/stdout, and can SIGKILL QEMU to simulate crashes.

## Code Style

- C/H files: 2 spaces, BSD style, no tabs (`.dir-locals.el`).
- Assembly (`.S`): 8 spaces.
- Makefile: tabs, 8-space indent.
- Build flags: `-Wall -Werror`, `-march=rv64gc`, `-mcmodel=medany`, `-ffreestanding`, `-nostdlib`.

## Architecture

### Boot Flow
`entry.S` (Machine mode, physical `0x80000000`) -> `start.c` (setup delegation, PMP, timer) -> `mret` to `main.c` (Supervisor mode). CPU 0 initializes all subsystems then calls `userinit()`. Other CPUs wait on `started`, then enable paging and enter the scheduler.

### Memory Management
- **Physical allocator** (`kalloc.c`): simple free-list of 4KB pages. Fills freed pages with `0x01`, allocated with `0x05`.
- **Virtual memory** (`vm.c`): RISC-V Sv39 three-level page tables. Kernel page table direct-maps all physical memory plus MMIO regions. User page table maps text/data, stack, heap, trapframe, and trampoline.
- **Lazy allocation**: `sys_sbrk` accepts a type argument (`SBRK_EAGER` or `SBRK_LAZY`). Lazy mode defers physical allocation until a page fault (`scause` 13/15) triggers `vmfault()`.

### Process Management
- Fixed process table of `NPROC` (64) entries. States: `UNUSED`, `USED`, `SLEEPING`, `RUNNABLE`, `RUNNING`, `ZOMBIE`.
- Per-CPU round-robin scheduler. Context switch via `swtch.S` (saves `ra`, `sp`, `s0-s11`).
- Trapframe holds all user registers plus kernel metadata (`kernel_satp`, `kernel_sp`, `kernel_trap`, `kernel_hartid`). `trampoline.S` is mapped at `TRAMPOLINE` in both user and kernel space for atomic user<->kernel transitions.

### Traps and System Calls
- User traps: `uservec` (in `trampoline.S`) saves registers, switches to kernel page table, calls `usertrap()`. Returns via `usertrapret()` -> `userret`.
- Kernel traps: `kernelvec.S` saves caller-saved registers, calls `kerneltrap()` for device/timer interrupts.
- Syscalls (`syscall.c`): dispatch table maps number to handler. Arguments read from trapframe (`a0-a5`). 21 syscalls defined in `kernel/syscall.h`.

### File System (layered with crash recovery)
- **Disk layout**: `[ boot | superblock | log | inode blocks | bitmap | data blocks ]`
- **Logging** (`log.c`): `begin_op()` / `end_op()` bracket FS syscalls. `log_write()` replaces `bwrite()`. Commit writes blocks to log, writes header (atomic commit point), installs to home locations, then clears log.
- **Orphan recovery**: `ireclaim()` in `fs.c` scans inodes at boot and frees any with `type != 0` but `nlink == 0`. Tested by `forphan`/`dorphan`.
- **Buffer cache** (`bio.c`): LRU cache of disk blocks.
- **Inode layer** (`fs.c`): on-disk and in-memory inode management. Locking protocol: `ilock()` before accessing metadata/data.
- **Files** (`file.c`): file descriptor abstraction over inodes, pipes, and devices.

### Devices
- **UART** (`uart.c`): 16550A serial port, interrupt-driven.
- **Console** (`console.c`): line-buffered input. `^P` dumps process table.
- **Virtio disk** (`virtio_disk.c`): MMIO interface to QEMU virtio-blk. Descriptor ring with 8 entries.
- **PLIC** (`plic.c`): platform-level interrupt controller routing to harts.

## Custom Modifications vs. Upstream xv6-riscv

- **Enhanced `printf`**: Both kernel and user `printf` support width, left-alignment, zero-padding, `+`/`-`/` ` sign flags, and length modifiers (`l`, `ll`, `z`, `t`). Tested by `user/printfmt.c`.
- **Enhanced `ls`**: `user/ls.c` upgraded with formatted output.
- **Lazy `sbrk`**: `sys_sbrk` takes a type argument; user library exposes `sbrk()` (eager) and `sbrklazy()`.
- **Orphan inode recovery**: `ireclaim()` cleans up orphaned inodes at boot.
- **Renamed kernel functions**: `fork` -> `kfork`, `exit` -> `kexit`, `wait` -> `kwait`, `exec` -> `kexec`, `kill` -> `kkill`. `sys_sleep` renamed to `sys_pause`.

## Key Files

- `kernel/entry.S`, `kernel/start.c`, `kernel/main.c`: boot and initialization.
- `kernel/vm.c`, `kernel/kalloc.c`: memory management.
- `kernel/proc.c`, `kernel/swtch.S`, `kernel/trampoline.S`: processes and context switching.
- `kernel/trap.c`, `kernel/kernelvec.S`: trap handling.
- `kernel/fs.c`, `kernel/log.c`, `kernel/bio.c`, `kernel/file.c`: file system.
- `kernel/syscall.c`, `kernel/syscall.h`: system call dispatch.
- `user/usys.pl`: generates `user/usys.S` (syscall stubs) — must be re-run if syscalls change.
- `mkfs/mkfs.c`: host tool to build `fs.img`.
- `test-xv6.py`: automated testing script.

## Important Invariants

- `p->lock` must be held when manipulating process state, `chan`, `killed`, `xstate`, `pid`.
- `wait_lock` must be held when manipulating `p->parent`.
- `ilock()` must be held before reading/writing inode metadata or content.
- File system operations must be wrapped in `begin_op()` / `end_op()`.
- `TRAPFRAME` sits at `TRAMPOLINE - PGSIZE` in every user address space.
- Kernel stacks are at `KSTACK(p)` with guard pages.
