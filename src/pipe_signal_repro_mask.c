/* pipe_signal_repro.c
 *
 * On IBM i PASE a pipe's read end can fail to see end-of-file after the only
 * writer closed the write end successfully, when signals are delivered to the
 * process while it runs.  Without signals it never happens.
 *
 * Each iteration: pipe(); a helper thread sleeps 0-2 ms, writes one byte and
 * close()s the write end (close() returns 0); the main thread polls and reads
 * the read end until it returns 0 (EOF).  Another thread sends SIGURG to the
 * process every <sig_usec> microseconds; the handler does nothing.  If EOF does
 * not arrive within <timeout> seconds the program reports what it can see and
 * exits with status 1.
 *
 * build:  /QOpenSys/pkgs/bin/gcc-12 -maix64 -pthread -O1 -o pipe_signal_repro pipe_signal_repro.c
 * run:    ./pipe_signal_repro_mask [sig_usec=200 [iterations=100000 [timeout=20 [mask_close=0]]]]
 *         ./pipe_signal_repro 0        # control: no signals, runs to completion
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
#include <sys/types.h>
#include <sys/stat.h>

static int sig_usec = 200, iterations = 100000, timeout_s = 20, mask_close;
static volatile int stop;
static volatile long nsig;

static void handler(int sig, siginfo_t *si, void *ctx) { (void)sig; (void)si; (void)ctx; }

static void *sender(void *arg) {
	(void)arg;
	while (!stop) {
		kill(getpid(), SIGURG);
		nsig++;
		usleep(sig_usec);
	}
	return NULL;
}

struct writer { int fd; unsigned delay_us; volatile int closed; };

static void *write_then_close(void *arg) {
	struct writer *w = arg;
	usleep(w->delay_us);
	while (write(w->fd, "x", 1) < 0 && errno == EINTR)
		;
	sigset_t blocked, saved;
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGURG);
	if (mask_close)
		pthread_sigmask(SIG_BLOCK, &blocked, &saved);
	int cr = close(w->fd);
	if (mask_close)
		pthread_sigmask(SIG_SETMASK, &saved, NULL);
	if (cr != 0)
		fprintf(stderr, "close of the write end failed: %s\n", strerror(errno));
	w->closed = 1;
	return NULL;
}

int main(int argc, char **argv) {
	if (argc > 1) sig_usec = atoi(argv[1]);
	if (argc > 2) iterations = atoi(argv[2]);
	if (argc > 3) timeout_s = atoi(argv[3]);
	if (argc > 4) mask_close = atoi(argv[4]);

	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGURG, &sa, NULL);

	pthread_t sig;
	if (sig_usec > 0)
		pthread_create(&sig, NULL, sender, NULL);

	unsigned seed = 1;
	for (int i = 0; i < iterations; i++) {
		int p[2];
		if (pipe(p) < 0) { perror("pipe"); return 2; }
		fcntl(p[0], F_SETFL, O_NONBLOCK);

		struct writer w = { p[1], rand_r(&seed) % 2000, 0 };
		pthread_t t;
		pthread_create(&t, NULL, write_then_close, &w);

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
				printf("pipe %d: no EOF after %d s\n", i, timeout_s);
				printf("  writer thread finished write() and close(): %s\n", w.closed ? "yes" : "no");
				printf("  fstat of the write end (fd %d): %s\n", p[1],
				       fstat(p[1], &st) == 0 ? "succeeds, descriptor still open" : strerror(errno));
				errno = 0;
				ssize_t n = read(p[0], b2, sizeof b2);
				printf("  read of the read end, writer thread not yet joined: n=%zd %s\n", n, n < 0 ? strerror(errno) : "");
				pthread_join(t, NULL);
				errno = 0;
				n = read(p[0], b2, sizeof b2);
				printf("  read of the read end, after joining the writer thread: n=%zd %s\n", n, n < 0 ? strerror(errno) : "");
				printf("  (EAGAIN: a writer still exists; 0: EOF; EBADF: read end invalid)\n");
				printf("  signals sent: %ld, pipes completed: %d\n", nsig, i);
				return 1;
			}
		}
		pthread_join(t, NULL);
		close(p[0]);
	}
	stop = 1;
	if (sig_usec > 0)
		pthread_join(sig, NULL);
	printf("%d pipes, all reached EOF, %ld signals sent\n", iterations, nsig);
	return 0;
}
