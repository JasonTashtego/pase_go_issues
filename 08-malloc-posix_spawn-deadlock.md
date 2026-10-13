# 8. `malloc`/`free` concurrent with `posix_spawn` in a multi-threaded process deadlocks

**Status:** seen as a by-product of another probe; not isolated into a minimal reproducer. *Unverified* whether `posix_spawn` itself or the child's startup is what blocks.

## Observation
`sem_fork_probe` runs threads that `malloc`, `sem_init`, `sem_destroy` and `free`, while other threads `posix_spawn` and `waitpid` a trivial program. In malloc mode all threads went to sleep, no children were running and the CPU time did not advance (observed for about 25 minutes, then killed by hand). With `nomalloc=1` (one preallocated array of `sem_t`, no `malloc` or `free` in the sem threads) it completed in 3 of 3 runs; malloc mode timed out in 3 of 3.

## Reproducer
`src/sem_fork_probe.c`: `gcc-12 -maix64 -pthread -O1 -o sem_fork_probe sem_fork_probe.c -lpthread`.
```
timeout 60 ./sem_fork_probe 20000 4 2 0   # args: count semthreads spawners nomalloc. Malloc mode: hangs, rc=124
timeout 60 ./sem_fork_probe 20000 4 2 1   # no malloc in the sem threads: completes
timeout 60 ./sem_fork_probe 20000 4 0 0   # control, no spawners
```
Always run it under `timeout`; without it the terminal stays blocked.
