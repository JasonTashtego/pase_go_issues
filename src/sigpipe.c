/* sigpipe: does a signal arriving around pipe()/write()/close() leave a pipe
 * with a phantom writer?  No Go involved.
 *
 * build (IBM i): /QOpenSys/pkgs/bin/gcc-12 -maix64 -pthread -O1 -o sigpipe sigpipe.c
 * run:           ./sigpipe [-t threads] [-n iters] [-s sig_usec] [-w wait_sec] [-x prog] [-c churn] [-k 0|1]
 *   -s 0 sends no signals (control run); default 200.
 *
 * Each worker: pipe(); a helper thread sleeps 0-2ms, writes "x", close(write
 * end); the worker waits with poll() for the read end to deliver "x" and then
 * EOF.  If EOF does not arrive within -w seconds the pipe is reported LOST and
 * the program exits 3.  A signal thread sends SIGURG to the process at the given
 * interval; the handler is empty (installed SA_RESTART|SA_SIGINFO like Go's).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <spawn.h>

static int nthreads = 8, iters = 20000, sigusec = 200, waitsec = 20;
static volatile int stop;
static volatile long total, nsig;

static void handler(int sig, siginfo_t *si, void *ctx) { (void)sig; (void)si; (void)ctx; }

static void *sender(void *arg) {
	(void)arg;
	while (!stop) {
		kill(getpid(), SIGURG);
		nsig++;
		usleep(sigusec);
	}
	return NULL;
}

static volatile long n_again, n_intr, n_poll0, n_pollerr, n_pollrdy;

struct wr { int fd; unsigned delay; volatile int done; };

static void *writer(void *arg) {
	struct wr *w = arg;
	usleep(w->delay);
	while (write(w->fd, "x", 1) < 0 && errno == EINTR)
		;
	int r = close(w->fd);
	if (r < 0)
		fprintf(stderr, "close of write end failed: %s\n", strerror(errno));
	w->done = 1;
	return NULL;
}

static pthread_mutex_t lostmu = PTHREAD_MUTEX_INITIALIZER;

static void lost(int rfd, struct wr *w) {
	pthread_mutex_lock(&lostmu); /* first reporter keeps the lock and exits */
	char b[16];
	struct stat st;
	ssize_t n = read(rfd, b, sizeof b);
	printf("LOST: no EOF within %ds. writer closed=%d, write fd %d fstat=%s\n",
	       waitsec, w->done, w->fd, fstat(w->fd, &st) == 0 ? "STILL OPEN" : "EBADF");
	printf("direct read: n=%zd errno=%s (EAGAIN = a writer reference still exists; 0 = EOF was available)\n",
	       n, n < 0 ? strerror(errno) : "-");
	printf("during the wait: read EAGAIN x%ld, read EINTR x%ld, poll timeouts x%ld, poll EINTR x%ld, poll ready x%ld\n", n_again, n_intr, n_poll0, n_pollerr, n_pollrdy);
	printf("signals sent so far: %ld, pipes ok: %ld\n", nsig, total);
	fflush(stdout);
	_exit(3);
}

static void *worker(void *arg) {
	unsigned seed = (unsigned)(long)arg * 7919u + 1;
	for (int i = 0; i < iters && !stop; i++) {
		int p[2];
		while (pipe(p) < 0) {
			if (errno != EINTR) { perror("pipe"); _exit(1); }
		}
		fcntl(p[0], F_SETFL, O_NONBLOCK);
		struct wr w = { p[1], rand_r(&seed) % 2000, 0 };
		pthread_t t;
		if (pthread_create(&t, NULL, writer, &w) != 0) { perror("pthread_create"); _exit(1); }
		int sawdata = 0, eof = 0;
		time_t start = time(NULL);
		while (!eof) {
			struct pollfd pf = { p[0], POLLIN, 0 };
			int r = poll(&pf, 1, 1000);
			if (r < 0 && errno != EINTR) { perror("poll"); _exit(1); }
			if (r < 0) n_pollerr++; else if (r == 0) n_poll0++; else n_pollrdy++;
			char b[16];
			for (;;) {
				ssize_t n = read(p[0], b, sizeof b);
				if (n > 0) { sawdata = 1; continue; }
				if (n == 0) { eof = 1; break; }
				if (errno != EAGAIN && errno != EINTR) {
					int e = errno;
					pthread_mutex_lock(&lostmu);
					printf("READ ERROR (not a lost wakeup): read fd %d errno=%s, writer closed=%d\n", p[0], strerror(e), w.done);
					fflush(stdout);
					_exit(4);
				}
				if (errno == EAGAIN) n_again++; else n_intr++;
				break; /* EAGAIN or EINTR */
			}
			if (!eof && time(NULL) - start >= waitsec) {
				pthread_join(t, NULL);
				lost(p[0], &w);
			}
		}
		(void)sawdata;
		pthread_join(t, NULL);
		close(p[0]);
		total++;
	}
	return NULL;
}

/* -x mode: fork+exec a trivial program with a CLOEXEC status pipe, as Go's
 * forkExec does, while signals arrive.  Reports any child that does not exit 0
 * and any status-pipe error.  The child's own stderr (for example "Could not
 * load program") appears on this program's stderr. */
static const char *xprog, *xarg = "--version";
static volatile long xfail;
static int nchurn, closeall, pmode;
static long memmb, resmb;
static int imode, umode, smode;
static const char *xdir;

/* -c N: N threads that open/close /dev/null and create/close pipes without
 * pause, so the descriptor table changes while other threads fork. */
static void *churner(void *arg) {
	(void)arg;
	while (!stop) {
		int f = open("/dev/null", O_RDONLY);
		int p[2];
		if (pipe(p) == 0) { close(p[0]); close(p[1]); }
		if (f >= 0) close(f);
	}
	return NULL;
}

extern char **environ;

/* -S 1: start the child with posix_spawn instead of fork+execve.  The child's
 * stdin (-i) and stdout/stderr (-p) are set up with file actions; the
 * descriptors involved are close-on-exec so nothing else leaks into the
 * child.  There is no status pipe: posix_spawn's return value reports a failed
 * exec, and a child that fails to load exits non-zero. */
/* -L 1: every descriptor-table operation of this program (pipe, open, close)
 * runs under a read lock, and the posix_spawn call itself under the write
 * lock, so no other thread changes the descriptor table while a spawn is in
 * progress. */
static pthread_rwlock_t fdlock = PTHREAD_RWLOCK_INITIALIZER;
static int lmode;
#define FDRD() do { if (lmode) pthread_rwlock_rdlock(&fdlock); } while (0)
#define FDWR() do { if (lmode) pthread_rwlock_wrlock(&fdlock); } while (0)
#define FDUN() do { if (lmode) pthread_rwlock_unlock(&fdlock); } while (0)

static void spawn_one(void) {
	int o[2] = { -1, -1 };
	int in = -1;
	FDRD();
	if (pmode) {
		while (pipe(o) < 0) {
			if (errno != EINTR) { perror("pipe"); _exit(1); }
		}
		fcntl(o[0], F_SETFD, FD_CLOEXEC);
		fcntl(o[1], F_SETFD, FD_CLOEXEC);
	}
	if (imode) {
		in = open("/dev/null", O_RDONLY);
		if (in < 0) { perror("open /dev/null"); _exit(1); }
		fcntl(in, F_SETFD, FD_CLOEXEC);
	}
	FDUN();
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	if (imode)
		posix_spawn_file_actions_adddup2(&fa, in, 0);
	if (pmode) {
		posix_spawn_file_actions_adddup2(&fa, o[1], 1);
		posix_spawn_file_actions_adddup2(&fa, o[1], 2);
	}
	char *av[] = { (char *)xprog, (char *)xarg, NULL };
	pid_t pid = 0;
	FDWR();
	int sr = posix_spawn(&pid, xprog, &fa, NULL, av, environ);
	FDUN();
	posix_spawn_file_actions_destroy(&fa);
	FDRD();
	if (pmode == 1)
		close(o[1]);
	if (imode == 1)
		close(in);
	FDUN();
	int st = 0;
	if (sr == 0) {
		while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
			;
	}
	FDRD();
	if (imode == 2)
		close(in);
	if (pmode == 2)
		close(o[1]);
	FDUN();
	char cap[256];
	cap[0] = 0;
	if (pmode) {
		ssize_t m, tot = 0;
		char tmp[256];
		while ((m = read(o[0], tmp, sizeof tmp)) != 0) {
			if (m < 0 && errno == EINTR)
				continue;
			if (m < 0)
				break;
			if (tot + m < (ssize_t)sizeof cap - 1) {
				memcpy(cap + tot, tmp, m);
				tot += m;
				cap[tot] = 0;
			}
		}
		FDRD();
		close(o[0]);
		FDUN();
	}
	if (sr != 0 || st != 0) {
		xfail++;
		printf("EXEC FAILURE #%ld after %ld ok: posix_spawn=%s exit status=0x%x captured=[%s]\n",
		       xfail, total, sr ? strerror(sr) : "ok", st, cap);
		fflush(stdout);
	}
	total++;
}

static void *xworker(void *arg) {
	(void)arg;
	for (int i = 0; i < iters && !stop; i++) {
		if (smode) {
			spawn_one();
			continue;
		}
		int p[2];
		while (pipe(p) < 0) {
			if (errno != EINTR) { perror("pipe"); _exit(1); }
		}
		fcntl(p[0], F_SETFD, FD_CLOEXEC);
		fcntl(p[1], F_SETFD, FD_CLOEXEC);
		int o[2] = { -1, -1 };
		if (pmode) {
			while (pipe(o) < 0) {
				if (errno != EINTR) { perror("pipe"); _exit(1); }
			}
		}
		int in = -1;
		if (imode) {
			/* like os/exec with a nil Stdin: a per-spawn /dev/null */
			in = open("/dev/null", O_RDONLY);
			if (in < 0) { perror("open /dev/null"); _exit(1); }
		}
		pid_t pid = fork();
		if (pid < 0) { perror("fork"); _exit(1); }
		if (pid == 0) {
			char *av[] = { (char *)xprog, (char *)xarg, NULL };
			extern char **environ;
			if (umode == 2) {
				/* handshake: tell the parent this child is running */
				char rb = 1;
				write(p[1], &rb, 1);
			}
			if (imode)
				dup2(in, 0);
			if (xdir && chdir(xdir) != 0) {
				int ce = errno;
				write(p[1], &ce, sizeof ce);
				_exit(126);
			}
			if (pmode) {
				dup2(o[1], 1);
				dup2(o[1], 2);
			}
			if (closeall == 1) {
				/* drop every inherited descriptor except 0-2 and the
				 * status pipe, one close() at a time */
				for (int fd = 3; fd < 256; fd++)
					if (fd != p[1])
						close(fd);
			} else if (closeall == 2) {
				/* exactly what Go's forkAndExecInChild does: close the
				 * descriptors below the status pipe, then F_CLOSEM
				 * everything above it.  A failing F_CLOSEM is reported
				 * through the status pipe so it shows as a failure. */
				for (int fd = 3; fd < p[1]; fd++)
					close(fd);
#ifndef F_CLOSEM
#define F_CLOSEM 10
#endif
				if (fcntl(p[1] + 1, F_CLOSEM, 0) < 0) {
					int ce = errno;
					write(p[1], &ce, sizeof ce);
					_exit(125);
				}
			}
			execve(xprog, av, environ);
			int e = errno;
			write(p[1], &e, sizeof e);
			_exit(127);
		}
		int st = 0;
		if (umode == 1) {
			/* keep the status pipe's write end open in the parent until the
			 * child has exited, then close it and read the result */
			while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
				;
		} else if (umode == 2) {
			/* keep it open until the child's first byte arrives */
			char rb;
			struct pollfd rp = { p[0], POLLIN, 0 };
			int rr;
			do { rr = poll(&rp, 1, waitsec * 1000); } while (rr < 0 && errno == EINTR);
			if (rr == 0) {
				printf("LOST: pid %d never sent its ready byte\n", (int)pid);
				fflush(stdout);
				_exit(3);
			}
			while (read(p[0], &rb, 1) < 0 && errno == EINTR)
				;
		}
		close(p[1]);
		if (imode == 1)
			close(in); /* os/exec closes its copy of the child's stdin after Start */
		if (pmode == 1)
			close(o[1]); /* like os/exec: parent drops the child's end at once */
		int e = 0;
		ssize_t n;
		struct pollfd pf = { p[0], POLLIN, 0 };
		int r;
		do { r = poll(&pf, 1, waitsec * 1000); } while (r < 0 && errno == EINTR);
		if (r == 0) {
			printf("LOST: status pipe of pid %d never reached EOF\n", (int)pid);
			fflush(stdout);
			_exit(3);
		}
		while ((n = read(p[0], &e, sizeof e)) < 0 && errno == EINTR)
			;
		close(p[0]);
		if (!umode) {
			while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
				;
		}
		if (imode == 2)
			close(in);
		char cap[256];
		cap[0] = 0;
		if (pmode) {
			if (pmode == 2)
				close(o[1]);
			ssize_t m, tot = 0;
			char tmp[256];
			while ((m = read(o[0], tmp, sizeof tmp)) != 0) {
				if (m < 0 && errno == EINTR)
					continue;
				if (m < 0)
					break;
				if (tot + m < (ssize_t)sizeof cap - 1) {
					memcpy(cap + tot, tmp, m);
					tot += m;
					cap[tot] = 0;
				}
			}
			close(o[0]);
		}
		if (n > 0 || st != 0) {
			xfail++;
			printf("EXEC FAILURE #%ld after %ld ok: status-pipe errno=%s exit status=0x%x captured=[%s]\n",
			       xfail, total, n > 0 ? strerror(e) : "none", st, cap);
			fflush(stdout);
		}
		total++;
	}
	return NULL;
}

int main(int argc, char **argv) {
	int c;
	for (int i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "-t")) nthreads = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-n")) iters = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-s")) sigusec = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-w")) waitsec = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-x")) xprog = argv[i + 1];
		else if (!strcmp(argv[i], "-a")) xarg = argv[i + 1];
		else if (!strcmp(argv[i], "-p")) pmode = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-m")) memmb = atol(argv[i + 1]);
		else if (!strcmp(argv[i], "-v")) resmb = atol(argv[i + 1]);
		else if (!strcmp(argv[i], "-i")) imode = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-u")) umode = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-S")) smode = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-L")) lmode = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-d")) xdir = argv[i + 1];
		else if (!strcmp(argv[i], "-c")) nchurn = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-k")) closeall = atoi(argv[i + 1]);
		else { fprintf(stderr, "usage: sigpipe [-t n] [-n iters] [-s usec] [-w sec] [-x prog] [-c churn_threads] [-k 0|1|2 child close-all: 1=close each fd, 2=Go-style F_CLOSEM] [-a arg] [-p 0|1|2 stdout+stderr via pipe; 1=parent closes child end at once, 2=after wait] [-m MB touch] [-v MB reserve] [-i 0|1|2 stdin=/dev/null; 1=parent closes at once, 2=after wait] [-d dir child chdir] [-u 1 keep status-pipe write end open until child exit | 2 until child sends a ready byte] [-S 1 posix_spawn instead of fork+execve; -d is not supported with it] [-L 1 lock descriptor operations against the spawn call, with -S 1]\n"); return 2; }
	}
	if (smode && xdir) {
		fprintf(stderr, "sigpipe: -d is not supported with -S\n");
		return 2;
	}
	printf("sigpipe: threads=%d iters=%d sig_interval=%dus wait=%ds\n", nthreads, iters, sigusec, waitsec);
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGURG, &sa, NULL);

	if (memmb > 0) {
		/* a parent with a large resident heap, like a big Go process */
		size_t sz = (size_t)memmb << 20;
		char *m = malloc(sz);
		if (!m) { perror("malloc"); return 1; }
		for (size_t o = 0; o < sz; o += 4096)
			m[o] = 1;
		printf("touched %ld MB\n", memmb);
	}
	if (resmb > 0) {
		/* reserved but unused address space, like Go's heap arenas */
		void *r = mmap(NULL, (size_t)resmb << 20, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_VARIABLE, -1, 0);
		if (r == MAP_FAILED) { perror("mmap"); return 1; }
		printf("reserved %ld MB at %p\n", resmb, r);
	}
	pthread_t sig, *ws = calloc(nthreads, sizeof *ws);
	if (sigusec > 0)
		pthread_create(&sig, NULL, sender, NULL);
	pthread_t *cs = calloc(nchurn > 0 ? nchurn : 1, sizeof *cs);
	for (c = 0; c < nchurn; c++)
		pthread_create(&cs[c], NULL, churner, NULL);
	for (c = 0; c < nthreads; c++)
		pthread_create(&ws[c], NULL, xprog ? xworker : worker, (void *)(long)(c + 1));
	for (c = 0; c < nthreads; c++)
		pthread_join(ws[c], NULL);
	stop = 1;
	for (c = 0; c < nchurn; c++)
		pthread_join(cs[c], NULL);
	if (sigusec > 0)
		pthread_join(sig, NULL);
	printf("sigpipe: %ld iterations, 0 lost, %ld exec failures, %ld signals sent\n", total, xfail, nsig);
	return 0;
}
