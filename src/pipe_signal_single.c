/* pipe_signal_single.c
 *
 * Single-threaded variant of pipe_signal_repro.c: no pthread_create anywhere.
 * Signals come from a SIGALRM interval timer.  Each iteration: pipe(), write one
 * byte, wait 0-2 ms, close() the write end (close() returns 0), then read the
 * read end until it returns 0 (EOF).  If EOF does not arrive within <timeout>
 * seconds the program reports what it can see and exits with status 1.
 *
 * build:  /QOpenSys/pkgs/bin/gcc-12 -maix64 -O1 -o pipe_signal_single pipe_signal_single.c
 * run:    ./pipe_signal_single [timer_usec=200 [iterations=100000 [timeout=20]]]
 *         ./pipe_signal_single 0       # control: no timer, runs to completion
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>

static volatile long nsig;

static void handler(int sig, siginfo_t *si, void *ctx) { (void)sig; (void)si; (void)ctx; nsig++; }

int main(int argc, char **argv) {
	int timer_usec = 200, iterations = 100000, timeout_s = 20;
	if (argc > 1) timer_usec = atoi(argv[1]);
	if (argc > 2) iterations = atoi(argv[2]);
	if (argc > 3) timeout_s = atoi(argv[3]);

	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGALRM, &sa, NULL);
	if (timer_usec > 0) {
		struct itimerval it = { { 0, timer_usec }, { 0, timer_usec } };
		setitimer(ITIMER_REAL, &it, NULL);
	}

	unsigned seed = 1;
	for (int i = 0; i < iterations; i++) {
		int p[2];
		if (pipe(p) < 0) { perror("pipe"); return 2; }
		fcntl(p[0], F_SETFL, O_NONBLOCK);

		while (write(p[1], "x", 1) < 0 && errno == EINTR)
			;
		struct timespec ts = { 0, (long)(rand_r(&seed) % 2000) * 1000 };
		nanosleep(&ts, NULL);
		int cr = close(p[1]);
		if (cr != 0)
			fprintf(stderr, "close of the write end failed: %s\n", strerror(errno));

		time_t start = time(NULL);
		int eof = 0;
		while (!eof) {
			struct pollfd pf = { p[0], POLLIN, 0 };
			poll(&pf, 1, 100);
			char b[8];
			for (;;) {
				ssize_t n = read(p[0], b, sizeof b);
				if (n > 0) continue;
				if (n == 0) eof = 1;
				break;			/* EOF, or EAGAIN/EINTR: wait again */
			}
			if (!eof && time(NULL) - start >= timeout_s) {
				struct stat st;
				char b2[8];
				printf("pipe %d: no EOF after %d s; close() of the write end returned %d\n", i, timeout_s, cr);
				printf("  fstat of the write end (fd %d): %s\n", p[1],
				       fstat(p[1], &st) == 0 ? "succeeds, descriptor still open" : strerror(errno));
				errno = 0;
				ssize_t n = read(p[0], b2, sizeof b2);
				printf("  read of the read end: n=%zd %s\n", n, n < 0 ? strerror(errno) : "");
				printf("  (EAGAIN: a writer still exists; 0: EOF; EBADF: read end invalid)\n");
				printf("  signals delivered: %ld, pipes completed: %d\n", nsig, i);
				return 1;
			}
		}
		close(p[0]);
	}
	printf("%d pipes, all reached EOF, %ld signals delivered\n", iterations, nsig);
	return 0;
}
