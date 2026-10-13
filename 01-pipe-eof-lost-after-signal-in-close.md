# 1. A signal that interrupts `close()` of a pipe's write end leaves a phantom writer

**Status:** reproduced in plain C on IBM i V7R5 (POWER10), single-threaded and multi-threaded. Worked around in Go by blocking signals around `close`.

## Symptom
`close()` of the only write end of a pipe returns 0 and the descriptor is gone (`fstat` on it gives `EBADF`), but the read end never sees end-of-file: `read` keeps returning `EAGAIN` (non-blocking) or blocks forever. The kernel still counts a writer. The pipe's inode matched no open descriptor in the process.

## Conditions
- Signals must be delivered to the process while it runs. With no signals, 160,000 pipes were all fine.
- A single thread is enough (SIGALRM interval timer).
- Blocking the signal on the **calling thread, around `close()` only**, removes the problem: 100,000 pipes with half a million signals delivered to other threads, none lost. Blocking around `pipe()`, `fcntl()` or `write()` does nothing.

## Reproducers
| Program | Build flags | What it does |
|---|---|---|
| `src/pipe_signal_single.c` (no threads) | `-maix64 -O1` | loop: `pipe`, write a byte, wait 0-2 ms, `close` the write end, read until EOF; SIGALRM interval timer |
| `src/pipe_signal_repro.c` | `-maix64 -pthread -O1` | same, with a helper thread doing the write and close, and another thread sending SIGURG |
| `src/sigpipe.c` | `-maix64 -pthread -O1` | multi-worker version (also used for issue 2) |
| `src/pipe_signal_mask.c` | `-maix64 -O1` | `pipe_signal_single` with the signal blocked around chosen calls (finds which call matters) |
| `src/pipe_signal_repro_mask.c` | `-maix64 -pthread -O1` | threaded version; blocks the signal only in the thread calling `close()` |

Compiler: `/QOpenSys/pkgs/bin/gcc-12`. `build.sh` builds everything into `bin/`.

## Commands and expected results
```
./pipe_signal_single 0                  # control, no timer: all 100000 pipes reach EOF
./pipe_signal_single 200                # timer every 200 us: "pipe N: no EOF after 20 s; close() ... returned 0;
                                        #   fstat of the write end: Bad file number; read of the read end: EAGAIN"
                                        #   (lost at pipe 224 in one run; arguments: timer_usec iterations timeout_s)
./pipe_signal_repro 0                   # control
./pipe_signal_repro 200                 # SIGURG every 200 us: a pipe is lost within a few hundred iterations
./pipe_signal_mask 0                    # same as pipe_signal_single (expect a lost pipe)
./pipe_signal_mask 8                    # signal blocked around close() of the write end only: 100000 pipes OK
./pipe_signal_mask 16                   # blocked around every call: OK
./pipe_signal_mask 1 ; ./pipe_signal_mask 2 ; ./pipe_signal_mask 4   # around pipe / fcntl / write: still lost
./pipe_signal_repro_mask 200 100000 20 1   # threaded, block only in the closing thread: OK (504,848 signals)
./sigpipe -t 8 -n 20000 -s 0 -w 20      # control: 159999 pipes, 0 lost
./sigpipe -t 8 -n 20000 -s 200 -w 20    # with SIGURG every 200 us: pipes lost (exit status 3)
```
`mask` for `pipe_signal_mask` is a sum: 1 pipe(), 2 fcntl(O_NONBLOCK), 4 write(), 8 close() of the write end, 16 all four.

## Not the cause (tested)
gcc or libpthread (the single-threaded program has no threads); EINTR from `close` (no errors are reported, and retrying close changes nothing); the Go netpoller (same result with plain blocking descriptors).

## Go-side effect
Go sends itself SIGURG to preempt goroutines, so `os/exec` hung in `Wait` with the child already gone. Fixed in the port by blocking all signals on the calling thread around `close` (`GOIBMI_CLOSEMASK=0` turns the fix off).
