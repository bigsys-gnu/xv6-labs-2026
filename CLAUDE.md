# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

xv6 — MIT 6.1810's teaching OS, a Unix v6 re-implementation for RISC-V — used for course lab work. The main branch is `riscv`; each lab is done on its own branch (e.g. `lab-mlfq`, `lab-priority`, `feature/fsdump`).

## Build and run

Requires a riscv64 cross toolchain (auto-detected by the Makefile: `riscv64-unknown-elf-`, `riscv64-linux-gnu-`, etc.) and `qemu-system-riscv64` >= 7.2.

```sh
make qemu          # build kernel + fs.img and boot xv6 in qemu
make qemu CPUS=1   # default is 3 CPUs
make clean
```

Exit qemu with `Ctrl-A x`. Note: `fs.img` is only rebuilt when user programs change; after changing on-disk fs layout code, `rm fs.img` to force a fresh image.

### Debugging

```sh
make qemu-gdb      # boots qemu halted, gdb stub on a per-user port
gdb                # in another terminal; reads generated .gdbinit
```

`./mkctags-cscope.sh` regenerates ctags/cscope indexes.

## Tests

`test-xv6.py` drives qemu programmatically (no manual shell interaction):

```sh
./test-xv6.py usertests        # full usertests suite (~10 min timeout)
./test-xv6.py -q usertests     # quick subset
./test-xv6.py forkfork         # any name not matching a test_* function in the
                               # script is passed as an argument to usertests,
                               # i.e. runs that single usertest
./test-xv6.py crash            # all crash-recovery tests (log + orphan)
./test-xv6.py log              # log crash/recovery test only
./test-xv6.py forphan          # orphaned-file recovery
./test-xv6.py dorphan          # orphaned-dir-entry recovery
```

The argument is a regex matched against `test_*` functions in the script. On failure, qemu output is saved to `test-xv6.out`. The crash tests kill qemu mid-operation and check recovery messages (`recovering`, `ireclaim`) on reboot — they use the helper user programs `logstress`, `forphan`, `dorphan`.

Inside the xv6 shell you can also run `usertests`, `usertests -q`, `usertests <testname>`, or `forktest` directly.

## Architecture

Monolithic kernel in `kernel/`, user programs in `user/`, `mkfs/mkfs` builds the initial disk image. `kernel/defs.h` declares every cross-file kernel function — add new kernel functions there.

- **Boot**: `entry.S` (per-hart entry) → `start.c` (M-mode setup, drop to S-mode) → `main.c` (init everything, first hart sets up, others spin then join) → `userinit` runs `user/init` → `sh`.
- **Traps**: user traps enter via `trampoline.S` (mapped at the same VA in every page table) → `trap.c:usertrap()`; kernel traps via `kernelvec.S`. Syscall dispatch: `syscall.c` indexes a function-pointer table by `a7`, implementations in `sysproc.c`/`sysfile.c`. User-pointer access must go through `copyin`/`copyout`/`argaddr`/`argint`.
- **Processes**: `proc.c` (process table, scheduler, fork/exit/wait, sleep/wakeup), context switch in `swtch.S`. Each CPU has a scheduler context; `sched()`/`scheduler()` swap through it.
- **Memory**: `kalloc.c` (free-list page allocator), `vm.c` (Sv39 page tables; kernel page table + per-process page tables). Layout constants in `memlayout.h`; trampoline and trapframe sit at the top of every user address space.
- **File system** (layered, bottom-up): `virtio_disk.c` (driver) → `bio.c` (buffer cache, sleeplocks) → `log.c` (write-ahead log; all fs syscalls bracket disk writes in `begin_op`/`end_op`) → `fs.c` (inodes, directories, block allocation) → `file.c` (file descriptors) → `sysfile.c` (syscalls). On-disk layout defined in `fs.h` and mirrored in `mkfs/mkfs.c`.
- **Locks**: `spinlock.c` (with push_off/pop_off interrupt nesting) and `sleeplock.c` (for locks held across disk I/O).

### Adding a syscall

1. Number in `kernel/syscall.h`, table entry + extern in `kernel/syscall.c`
2. Implementation in `kernel/sysproc.c` or `kernel/sysfile.c`
3. Declaration in `user/user.h`, stub via `entry("name")` in `user/usys.pl`

### Adding a user program

Create `user/foo.c` with a `main`, add `$U/_foo\` to `UPROGS` in the Makefile. It gets compiled, linked against ulib, and packed into `fs.img`.
