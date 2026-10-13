#!/bin/sh
# Build every reproducer in src/ into ./bin (run on IBM i PASE, gcc-12 from the
# IBM open source repository).
CC=${CC:-/QOpenSys/pkgs/bin/gcc-12}
mkdir -p bin
for f in src/*.c; do
	n=$(basename "$f" .c)
	case $n in
	# programs that use threads, semaphores or rwlocks need -pthread
	pipe_signal_repro|pipe_signal_repro_mask|sigpipe|open_close_race|open_slot_race|exec_threads|sem_fork_probe)
		flags="-maix64 -pthread -O1" ;;
	*)	flags="-maix64 -O1" ;;
	esac
	echo "$CC $flags -o bin/$n $f"
	$CC $flags -o bin/$n "$f" || echo "FAILED: $n"
done
