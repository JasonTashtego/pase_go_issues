/* open_close_race.c
 *
 * Can PASE run open() and close() concurrently in one process?  Go's cmd/vet
 * sometimes gets EBADF from read() on a file it has just opened successfully,
 * while other goroutines open and close files.  Each thread loops: open a file,
 * read a few bytes, close it.  Any read() that fails is counted and reported.
 *
 * build:  /QOpenSys/pkgs/bin/gcc-12 -maix64 -pthread -O1 -o open_close_race open_close_race.c
 * run:    ./open_close_race [threads=8 [iterations=200000 [mode=0 [file]]]]
 *   mode 0: no locking
 *   mode 1: one mutex held around every open() and around every close()
 *           (so an open never overlaps a close or another open; reads are not locked)
 *   mode 2: one mutex held around the whole open, read, close sequence
 * The default file is this program's own executable.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

static int nthreads = 8, iterations = 200000, mode = 0;
static const char *path;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static volatile long read_fail, open_fail, close_fail;
static volatile int printed;

#define LOCK()   do { if (mode) pthread_mutex_lock(&mu); } while (0)
#define UNLOCK() do { if (mode) pthread_mutex_unlock(&mu); } while (0)

static void *worker(void *arg) {
	(void)arg;
	for (int i = 0; i < iterations; i++) {
		char b[16];
		if (mode == 2) pthread_mutex_lock(&mu);
		if (mode == 1) pthread_mutex_lock(&mu);
		int fd = open(path, O_RDONLY);
		if (mode == 1) pthread_mutex_unlock(&mu);
		if (fd < 0) {
			open_fail++;
			if (mode == 2) pthread_mutex_unlock(&mu);
			continue;
		}
		if (mode == 2) { /* keep the lock */ }
		ssize_t n = read(fd, b, sizeof b);
		if (n < 0) {
			read_fail++;
			if (printed++ < 5)
				printf("read of fd %d just returned by open: %s\n", fd, strerror(errno));
		}
		if (mode == 1) pthread_mutex_lock(&mu);
		if (close(fd) != 0) close_fail++;
		if (mode == 1) pthread_mutex_unlock(&mu);
		if (mode == 2) pthread_mutex_unlock(&mu);
	}
	return NULL;
}

int main(int argc, char **argv) {
	path = argv[0];
	if (argc > 1) nthreads = atoi(argv[1]);
	if (argc > 2) iterations = atoi(argv[2]);
	if (argc > 3) mode = atoi(argv[3]);
	if (argc > 4) path = argv[4];
	printf("threads=%d iterations=%d mode=%d file=%s\n", nthreads, iterations, mode, path);
	pthread_t *t = calloc(nthreads, sizeof *t);
	for (int i = 0; i < nthreads; i++)
		pthread_create(&t[i], NULL, worker, NULL);
	for (int i = 0; i < nthreads; i++)
		pthread_join(t[i], NULL);
	printf("%ld read failures, %ld open failures, %ld close failures in %d open/read/close cycles\n",
	       read_fail, open_fail, close_fail, nthreads * iterations);
	return (read_fail || open_fail || close_fail) ? 1 : 0;
}
