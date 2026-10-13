/* pipe_signal_mask.c
 *
 * pipe_signal_single.c with the timer signal blocked (sigprocmask) around chosen
 * calls, to find which call the signal has to interrupt for the pipe to lose
 * its EOF.  Pending signals are delivered when the signal is unblocked again.
 *
 * build:  /QOpenSys/pkgs/bin/gcc-12 -maix64 -O1 -o pipe_signal_mask pipe_signal_mask.c
 * run:    ./pipe_signal_mask <mask> [timer_usec=200 [iterations=100000 [timeout=20]]]
 *   mask is a sum of:  1 = around pipe()          2 = around fcntl(O_NONBLOCK)
 *                      4 = around write()         8 = around close() of the write end
 *                      16 = around every call (pipe, fcntl, write, close)
 *   ./pipe_signal_mask 0      same as pipe_signal_single (expect a lost pipe)
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
static int mask;

static void handler(int sig, siginfo_t *si, void *ctx) { (void)sig; (void)si; (void)ctx; nsig++; }

static void block(int bit, int on) {
	if (!(mask & bit) && !(mask & 16))
		return;
	sigset_t s;
	sigemptyset(&s);
	sigaddset(&s, SIGALRM);
	sigprocmask(on ? SIG_BLOCK : SIG_UNBLOCK, &s, NULL);
}

int main(int argc, char **argv) {
	int timer_usec = 200, iterations = 100000, timeout_s = 20;
	if (argc < 2) { fprintf(stderr, "usage: pipe_signal_mask mask [timer_usec [iterations [timeout]]]\n"); return 2; }
	mask = atoi(argv[1]);
	if (argc > 2) timer_usec = atoi(argv[2]);
	if (argc > 3) iterations = atoi(argv[3]);
	if (argc > 4) timeout_s = atoi(argv[4]);

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
		block(1, 1);
		int pr = pipe(p);
		block(1, 0);
		if (pr < 0) { perror("pipe"); return 2; }
		block(2, 1);
		fcntl(p[0], F_SETFL, O_NONBLOCK);
		block(2, 0);

		block(4, 1);
		while (write(p[1], "x", 1) < 0 && errno == EINTR)
			;
		block(4, 0);
		struct timespec ts = { 0, (long)(rand_r(&seed) % 2000) * 1000 };
		nanosleep(&ts, NULL);
		block(8, 1);
		int cr = close(p[1]);
		block(8, 0);

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
				printf("mask %d: pipe %d: no EOF after %d s; close() returned %d\n", mask, i, timeout_s, cr);
				printf("  fstat of the write end (fd %d): %s\n", p[1],
				       fstat(p[1], &st) == 0 ? "succeeds, descriptor still open" : strerror(errno));
				errno = 0;
				ssize_t n = read(p[0], b2, sizeof b2);
				printf("  read of the read end: n=%zd %s\n", n, n < 0 ? strerror(errno) : "");
				printf("  signals delivered: %ld, pipes completed: %d\n", nsig, i);
				return 1;
			}
		}
		close(p[0]);
	}
	printf("mask %d: %d pipes, all reached EOF, %ld signals delivered\n", mask, iterations, nsig);
	return 0;
}
