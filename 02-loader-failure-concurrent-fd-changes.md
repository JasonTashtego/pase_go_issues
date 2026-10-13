# 2. Child fails to load ("-1 9", EBADF) when another thread changes the descriptor table during fork+exec / posix_spawn

**Related:** issue 9 (same family). **Status:** reproduced in C, but only with a large executable (the Go `compile` binary) as the child. A stock IBM binary (`gcc-12 --version`) never failed. A smaller trigger binary has not been found, so the reproducer needs a Go toolchain's `compile`. **Open item:** find or build a self-contained large executable to send with the case.

## Symptom
The child prints this and exits with status 255:
```
Could not load program <path>: System error - error data is: -1 9 (64002005)
```
`posix_spawn` itself returns 0. In Go this made 50-60% of toolchain builds fail with the default number of threads.

## Conditions (all measured)
- ONE multi-threaded parent starting children from several threads at once, **and** other threads of that parent creating, duplicating or closing descriptors meanwhile.
- Eight threads, `fork`+`execve`: about 15% of starts fail. `posix_spawn`: about 30%.
- A single thread, or eight separate single-threaded processes (even with the same executable), never fail.
- A process-wide read/write lock makes it go away: every `pipe`, `open` and `close` takes the read lock, the `posix_spawn` call takes the write lock. 0 failures in 1,199 spawns, against 33, 10 and 20 per 400 without the lock (interleaved runs).
- Not signals (none are sent), not parent memory size, not thread count, not stdio setup.

## Reproducer
`src/sigpipe.c` (`gcc-12 -maix64 -pthread -O1`). `T` is the Go compile binary, for example `$GOROOT/pkg/tool/aix_ppc64/compile`; `-a -V` makes the child run `compile -V`.
```
T=/path/to/compile
./sigpipe -t 8 -n 50 -s 0 -c 0 -p 1 -i 1 -S 1 -L 0 -a -V -x $T | grep -E 'iterations|EXEC FAILURE'
    # no lock: "400 iterations, ... 33 exec failures" (varies run to run: 33, 10, 20 seen)
./sigpipe -t 8 -n 50 -s 0 -c 0 -p 1 -i 1 -S 1 -L 1 -a -V -x $T | grep -E 'iterations|EXEC FAILURE'
    # with the lock: 0 failures
./sigpipe -t 1 -n 50 -s 0 -c 0 -S 1 -a -V -x $T     # one thread: 0 failures
```
Flags: `-t` worker threads, `-n` iterations per thread, `-s 0` no signals, `-c N` extra churn threads that open and close `/dev/null` and pipes, `-p 1` child stdout and stderr through a pipe, `-i 1` stdin from `/dev/null`, `-S 1` use `posix_spawn` (default is `fork`+`execve`), `-L 1` lock descriptor operations against the spawn call, `-a ARG` child argument, `-x PROG` the child. A bad flag prints the full usage.

The exact numbers above come from the working notes; run the three commands interleaved a few times before quoting them.

## Workaround in Go
A process-wide `sync.RWMutex` (`syscall/exec_aix_fdlock.go`): read-locked in `Open`, `Close`, `Dup`, `Pipe`, `Socket`, `Accept` and so on, write-locked while `forkExec` creates the child. `GOIBMI_FDLOCK=0` turns it off; `make.bash` then fails within seconds.
