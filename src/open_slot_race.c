/* open_slot_race.c
 *
 * On IBM i PASE, go vet sometimes gets EBADF from read() on a descriptor that
 * open() has just returned.  The descriptor is always 1.  vet runs with
 * descriptors 1 and 2 on the same pipe, closes descriptor 1 early, and then
 * many threads open a small file, read it and close it again, so almost every
 * open() lands on slot 1.  Serializing open() against the other descriptor
 * calls removes the failure; serializing close() does not.
 *
 * This program recreates those conditions in C.  Every thread loops: open the
 * file, read a few bytes, close.  A read that fails is counted and the first
 * ones are reported with the descriptor number.
 *
 * build:  /QOpenSys/pkgs/bin/gcc-12 -maix64 -pthread -O1 -o open_slot_race open_slot_race.c
 * run:    ./open_slot_race [-t threads=4] [-n iterations=200000] [-m mode=0]
 *                          [-p 0|1 pipe on 1 and 2 =1] [-c 0|1 close(1) =1]
 *                          [-h held=4] [-f file]
 *   -m mode    0 no locking
 *              1 one mutex around every open()
 *              2 one mutex around every close()
 *              3 one mutex around every open() and every close()
 *              4 one mutex around the whole open, read, close sequence
 *   -p 1       before the loops, a pipe is made and its write end becomes
 *              descriptors 1 and 2 (like go vet run by the go command)
 *   -c 1       descriptor 1 is closed before the loops (like the finalizer of
 *              the old os.Stdout in vet), so it is the lowest free slot
 *   -h N       N descriptors are kept open on /dev/null, above the pipe
 *   -f file    the file to read; the default is this program's executable
 * Reports go to a duplicate of the original standard output, kept above 20.
 *
 * Suggested runs, interleaved, a few times each (failures are rare per open,
 * so use many iterations):
 *   ./open_slot_race -m 0            all conditions, no locking
 *   ./open_slot_race -m 1            expected clean if open() against open() is the race
 *   ./open_slot_race -m 2            expected to still fail if close() is not involved
 *   ./open_slot_race -m 0 -p 0 -c 0  none of vet's descriptor conditions
 *   ./open_slot_race -m 0 -c 0       pipe on 1 and 2 but no free slot 1
 *   ./open_slot_race -m 0 -t 1       one thread: no concurrency
 * Exit status 1 if any read failed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

static int nthreads = 4, iterations = 200000, mode = 0, usepipe = 1, close1 = 1, held = 4;
static const char *path;
static FILE *out;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static volatile long read_fail, open_fail, on1, opens;

static void *worker(void *arg) {
	long local1 = 0;
	(void)arg;
	for (int i = 0; i < iterations; i++) {
		char b[16];
		int fd;
		ssize_t n;

		if (mode == 4) pthread_mutex_lock(&mu);
		if (mode == 1 || mode == 3) pthread_mutex_lock(&mu);
		fd = open(path, O_RDONLY);
		if (mode == 1 || mode == 3) pthread_mutex_unlock(&mu);
		if (fd < 0) {
			int e = errno;
			long k = __sync_fetch_and_add(&open_fail, 1);
			if (k < 5) {
				fprintf(out, "open failed: errno=%d (%s)\n", e, strerror(e));
				fflush(out);
			}
			if (mode == 4) pthread_mutex_unlock(&mu);
			continue;
		}
		if (fd == 1) local1++;
		n = read(fd, b, sizeof b);
		if (n <= 0) {
			int e = errno;
			long k = __sync_fetch_and_add(&read_fail, 1);
			if (k < 10) {
				int fl = fcntl(fd, F_GETFL);
				fprintf(out, "read failed: fd=%d n=%zd errno=%d (%s) fcntl(F_GETFL)=%d\n",
				        fd, n, e, n < 0 ? strerror(e) : "EOF", fl);
				fflush(out);
			}
		}
		if (mode == 2 || mode == 3) pthread_mutex_lock(&mu);
		close(fd);
		if (mode == 2 || mode == 3) pthread_mutex_unlock(&mu);
		if (mode == 4) pthread_mutex_unlock(&mu);
	}
	__sync_fetch_and_add(&on1, local1);
	__sync_fetch_and_add(&opens, iterations);
	return NULL;
}

int main(int argc, char **argv) {
	path = argv[0];
	for (int i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "-t")) nthreads = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-n")) iterations = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-m")) mode = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-p")) usepipe = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-c")) close1 = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-h")) held = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-f")) path = argv[i + 1];
		else {
			fprintf(stderr, "usage: open_slot_race [-t threads] [-n iterations] [-m mode 0-4] [-p 0|1] [-c 0|1] [-h held] [-f file]\n");
			return 2;
		}
	}
	if (nthreads < 1 || nthreads > 256) { fprintf(stderr, "bad -t\n"); return 2; }

	/* reports go to a copy of the original standard output, above 20 */
	int o = fcntl(1, F_DUPFD, 20);
	if (o < 0) { perror("F_DUPFD"); return 2; }
	out = fdopen(o, "w");
	if (out == NULL) { perror("fdopen"); return 2; }
	setvbuf(out, NULL, _IONBF, 0);

	if (usepipe) {
		int p[2];
		if (pipe(p) < 0) { perror("pipe"); return 2; }
		if (dup2(p[1], 1) < 0 || dup2(p[1], 2) < 0) { perror("dup2"); return 2; }
		if (p[1] > 2) close(p[1]);
		/* p[0] stays open and is never read: nothing writes to the pipe */
	}
	for (int i = 0; i < held; i++) {
		int f = open("/dev/null", O_RDONLY);
		if (f < 0) { perror("open /dev/null"); return 2; }
	}
	if (close1) close(1);

	fprintf(out, "open_slot_race: threads=%d iterations=%d mode=%d pipe=%d close1=%d held=%d file=%s\n",
	        nthreads, iterations, mode, usepipe, close1, held, path);

	/* the file must be readable here, with slot 1 as free as it will be */
	int probe = open(path, O_RDONLY);
	char pb[16];
	if (probe < 0 || read(probe, pb, sizeof pb) <= 0) {
		fprintf(out, "cannot read %s before starting: errno %d (%s)\n", path, errno, strerror(errno));
		return 2;
	}
	close(probe);

	pthread_t t[256];
	for (int i = 0; i < nthreads; i++)
		if (pthread_create(&t[i], NULL, worker, NULL) != 0) { perror("pthread_create"); return 2; }
	for (int i = 0; i < nthreads; i++)
		pthread_join(t[i], NULL);

	fprintf(out, "%ld opens, %ld of them returned fd 1, %ld open failures, %ld read failures\n",
	        opens, on1, open_fail, read_fail);
	return read_fail != 0;
}
