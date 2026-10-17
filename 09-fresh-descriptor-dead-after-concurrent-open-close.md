# 9. A descriptor returned by `open()` can be unusable (`read` gives EBADF) when other threads open and close files at the same time

**Status:** reproduced in plain C (`src/open_slot_race.c`, about 2% of opens give an unusable descriptor with 4 threads) and in Go (`go vet`). A mutex around `open` removes it in both; a mutex around `close` does not.

## Symptom
`go vet` fails a package in about 1% of processes (about 20-30% with the timing diagnostics enabled) with:
```
internal/bytealg: read /.../equal_arm64.s: bad file number
```
The file was opened successfully and the very next `read` returned EBADF. The descriptor is always **fd 1**. At the moment of failure it is half valid: `fcntl(F_GETFD)` succeeds, `fcntl(F_GETFL)` returns -1, `fstat` fails, `read` gives EBADF, and the descriptor shows in the list of open descriptors.

## How fd 1 gets reused
`vet` replaces `os.Stdout`; the old file object is finalized, which closes descriptor 1. From then on analyzer goroutines (concurrent threads) read many small files with `open`, `read`, `close`, and each new file gets the lowest free slot, which is 1, about every 100 microseconds. Descriptors 3 to 6 are used by other goroutines at the same time.

## Evidence (timeline from instrumented runs)
Every `Close(0..2)` and every `open` that returns 0..2 is timestamped (microseconds):

| run | previous `close(1)` returned | `open` returned 1 | `read` gave EBADF |
|---|---|---|---|
| 1 | 46.472474 | 46.478617 | 46.478667 (+50 us) |
| 3 | 239.835036 | 239.841186 | 239.841244 (+58 us) |
| 5 (second hit) | 371.316398 | 371.322496 | 371.322570 (+74 us) |
| 5 (first hit) | 371.309839 | 371.309941 | 371.309957 (+16 us) |

- No `close` of descriptor 1 occurs between the `open` and the failing `read`. Closes of descriptor 1 go through one wrapper, and the other places in the process that could close it were checked (only the startup random-seed read, and the child side of fork).
- The old `os.Stdout` close happened about 15 ms and 25 open/close cycles earlier in each run, so it is not racing the failing open.

## Control experiment (same build, same machine, `go vet` of three packages in a loop)
| Descriptor calls | Result |
|---|---|
| unmodified (read lock on Open, Close, ...) | 5 failures in 17 runs (29%); earlier build: 5 in 26 |
| every descriptor call takes a process-wide **exclusive** lock (`GOIBMI_FDLOCK=w`) | **0 failures in 100 runs**; same speed (13 s per run) |
| unmodified control, rerun on the build used for the next two rows | 5 failures in 27 runs (19%) |
| only `Close` takes the exclusive lock (`GOIBMI_FDLOCK=c`; `open` and everything else take the shared lock, so `open` is still excluded from `close`) | 5 failures in 26 runs (19%): **no effect** |
| only `Open` and `Openat` take the exclusive lock (`GOIBMI_FDLOCK=o`; `open` is excluded from every other descriptor call, including other `open`s) | **0 failures in 100 runs** |

If the failure rate were still 29%, 0 in 100 would have probability about 10^-15.

**What the three modes show.** Excluding `open` from `close` is not enough (mode `c` still fails at the control rate). Excluding `open` from everything else, including other `open` calls, removes the failure (mode `o`). So the failing interaction does not need a concurrent `close`: it is an `open` racing with another `open`, or with another descriptor-creating call (`pipe`, `dup`, `socket`; the vet process makes very few of those, so another `open` is the likely partner, *unverified*). The earlier timeline reading ("the previous `close(1)` leaves the slot half torn down") is not supported.

## Interpretation (*unverified*)
Two threads calling `open()` at the same time can both be handed the same lowest free slot (here 1), or otherwise corrupt it, so one of them gets a half-valid descriptor. Same class as issue 2 (the descriptor table is not safe against concurrent changes by several threads).

## C reproducer attempts
- `src/open_slot_race.c` (**reproduces the failure**, see below): recreates vet's conditions: a pipe on descriptors 1 and 2, `close(1)`, several threads in an `open`, `read`, `close` loop so that most opens land on slot 1. Build `gcc-12 -maix64 -pthread -O1`. Options: `-t` threads, `-n` iterations, `-m` mode (0 no lock, 1 mutex around `open`, 2 around `close`, 3 around both, 4 around the whole sequence), `-p 0|1` pipe on 1 and 2, `-c 0|1` close descriptor 1, `-h N` held descriptors. Runs to try, interleaved: `-m 0` (expect failures if it reproduces), `-m 1` (expect clean if `open` against `open` is the race), `-m 2` (expect still failing), `-m 0 -p 0 -c 0` and `-m 0 -c 0` (which of vet's conditions are needed), `-m 0 -t 1` (no concurrency). Prints how many opens returned fd 1 and how many reads failed.
- `src/open_close_race.c`: threads that open a file, read a few bytes, and close; reports every failing `read`. Modes: 0 no lock, 1 lock around every `open` and every `close`, 2 lock around the whole sequence. Build `gcc-12 -maix64 -pthread -O1`, run `./open_close_race [threads=8 [iterations=200000 [mode=0 [file]]]]`. It has no descriptor-1 reuse (all threads keep their descriptors above 2); its result is not recorded, and `open_slot_race.c` replaces it.
- Go programs that did not reproduce it (all 0 failures): concurrent open and read of the same files (one million reads), the same with a signal storm, closing the write end of a pipe while other threads open files (31,450 opens landed on fd 1), spawning children with a shared pipe.

## C reproducer result (open_slot_race, 4 threads, 200,000 opens each, pipe on 1 and 2, close(1), 4 held descriptors; 3 rounds each)
| Mode | Read failures per 800,000 opens | Open failures |
|---|---|---|
| `-m 0` no lock | 15,887 / 16,176 / 16,806 | about 1,250 |
| `-m 1` mutex around `open` only | **0, 0, 0** (about half of the opens returned fd 1) | 0 |
| `-m 2` mutex around `close` only | 17,591 / 18,065 / 16,667 | about 1,250 |

A failed read gives EBADF; `fcntl(F_GETFL)` on that descriptor returns -1 (a few times 0). The bad descriptors are mostly 1 but also 8, 9 and 10, so slot 1 is not special. This is a plain-C reproducer with no Go in it: concurrent `open()` calls in one process hand out unusable descriptors, and some fail outright. The earlier Go and C probes that did concurrent opens without a pipe on 1 and 2 and without a freed slot 1 saw no failures; the next table shows which of those two conditions matter.

**Which conditions are needed** (one round each, 4 threads unless noted):

| Run | Opens that returned fd 1 | Read failures |
|---|---|---|
| `-m 0` (pipe on 1 and 2, `close(1)`) | about 165,000 of 800,000 | about 16,000 |
| `-m 0 -c 0` (pipe, but descriptor 1 stays open) | 0 | **0** |
| `-m 0 -p 0 -c 0` (neither) | 0 | **0** |
| `-m 0 -t 1` (one thread, pipe, `close(1)`) | 200,000 of 200,000 | **0** |

So it needs both a free low slot and several threads. With no free slot below the held descriptors each thread keeps getting back its own slot and nothing contends; with a free slot 1 all threads ask for the same lowest free descriptor at the same moment. Interpretation (*unverified*): two concurrent `open()` calls that are handed the same lowest free slot corrupt it. That also explains failures on 8, 9 and 10, and why a mutex around `open` alone cures it. Real programs hit this when a low descriptor is free, for example a daemon started with stdin closed, or `go vet`, which closes descriptor 1 when it replaces `os.Stdout`.

## Effect and workaround
`go vet` (run by `go test` and `all.bash`) reported a spurious `[build failed]` in about 1% of packages.

Workaround in the Go port (`syscall/open_aix.go`, commit 8596c01e43 on the `iseries-1.26.9` branch): `Open` takes a mutex around the `open()` call. A blocking `open` of a FIFO must not hold the mutex, since the other end may be opened by another goroutine of the same process, so the open under the mutex always carries `O_NONBLOCK`, which is cleared again afterwards; a FIFO opened for reading is closed and opened again outside the mutex, and so is a failure with `ENXIO` or `EAGAIN`. FIFO opens are therefore not serialized against other opens. `GOIBMI_OPENLOCK=0` turns it off.

Measured on the IBM i:

| Check | Result |
|---|---|
| `go vet` capture, 100 runs, fix on | 0 failures (without it 19 to 29%) |
| Go stress, 4 goroutines, fd 1 free, 400,000 opens | fix off 3,349 failures; fix on 0 |
| FIFO opens | both ends still wait for the other end and then return |
| Cost of one open+close, single thread | 20 us without, 37 us with (two to three extra libc calls per open) |
