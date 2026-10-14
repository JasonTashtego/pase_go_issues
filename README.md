# IBM i PASE issues found while porting Go

Standalone C reproducers, collected for an IBM support case.

## Test system

| Item | Value |
|---|---|
| `uname -a` | `OS400 DEV5 5 7 00780005CAB1 powerpc Os` (IBM i 7.5) |
| Release | `V7R5M0` (`QSYS2.SOFTWARE_PRODUCT_INFO`, product 5770SS1, load state 90 = installed) |
| Hardware | POWER10 |
| Environment | PASE `aix/ppc64`, 64-bit programs (`-maix64`) |
| Compiler | `gcc-12 (IBM i) 12.4.0 20240620 (IBM 12.4.0-3)`, `/QOpenSys/pkgs/bin/gcc-12` |
| Newest 5770SS1 PTFs (status `APPLIED`) | SJ11190 (created 2026-08-13 13:49), SJ11149 (2026-08-11), SJ11125 (2026-08-10), SJ11102 (2026-08-08), SJ11098 (2026-08-08) |
| PTF groups (`QSYS2.GROUP_PTF_INFO`, all `INSTALLED`) | `SF99957` Technology Refresh: level 8 (also listed: 3). `SF99750` cumulative package: level 26197, C6197750 (also listed: 23306, C3306750). `SF99751` all groups except cumulative and MQ: level 9 (also 5). `SF99959` Group HIPER: level 106 (also 44). `SF99956` Technology Refresh plus recommended groups: level 1 |

Note: each group appears twice with two levels; the higher one is taken as the current level (*unverified*, the lower may be a previous level that is still on the system). The five PTFs above are the newest from `QSYS2.PTF_INFO`.

## Build
```
./build.sh            # builds every src/*.c into bin/ with -maix64 -O1 (and -pthread where needed)
```

## Index

| # | Issue | Strength | Reproducers (`src/`) |
|---|---|---|---|
| [1](01-pipe-eof-lost-after-signal-in-close.md) | A signal interrupting `close()` of a pipe write end leaves a phantom writer; the reader never sees EOF | reproduced, single-threaded C | `pipe_signal_single.c`, `pipe_signal_repro.c`, `pipe_signal_mask.c`, `pipe_signal_repro_mask.c`, `sigpipe.c` |
| [2](02-loader-failure-concurrent-fd-changes.md) | Child fails to load (`-1 9`, EBADF) when another thread changes the descriptor table during fork+exec / posix_spawn | reproduced, needs a large executable as the child | `sigpipe.c` |
| [3](03-at-calls-einval.md) | Every `*at` call fails with EINVAL for a real directory descriptor | reproduced, deterministic | `at_probe.c` |
| [4](04-execve-fails-in-multithreaded-process.md) | `execve` fails with EPERM in a multi-threaded process | reproduced, deterministic | `exec_threads.c` |
| [5](05-users-and-groups.md) | Users and groups: no passwd/group files, `getgrset` returns a static buffer | observed | `getgrset_probe.c`, `group_probe.c` |
| [6](06-getaddrinfo-accepts-invalid-service.md) | `getaddrinfo(NULL, "☺")` succeeds with port 0 | reproduced, deterministic | `getserv_probe.c`, `getaddr_probe.c` |
| [7](07-network-interfaces.md) | No `getifaddrs`; interface ioctls need the IP address text as the name | reproduced | `if_probe.c`, `if_probe2.c` |
| [8](08-malloc-posix_spawn-deadlock.md) | `malloc`/`free` with concurrent `posix_spawn` deadlocks | seen in 3 of 3 runs of a probe; no minimal reproducer | `sem_fork_probe.c` |
| [9](09-fresh-descriptor-dead-after-concurrent-open-close.md) | A descriptor returned by `open()` is unusable (`read` EBADF) when other threads open and close at the same time | reproduced in C (about 2% of opens with 4 threads; 0 with a mutex around `open`, no change with one around `close`) and in Go | `open_slot_race.c` |
