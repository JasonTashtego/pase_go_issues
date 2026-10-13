/* exec_threads.c
 *
 * Does execve() of the running process fail on IBM i PASE when the process has
 * more than one thread?  Starts <nthreads> extra threads that just sleep, then
 * replaces the process image with <program>.  If execve returns, the errno is
 * printed.
 *
 * build:  /QOpenSys/pkgs/bin/gcc-12 -maix64 -pthread -O1 -o exec_threads exec_threads.c
 * run:    ./exec_threads 0 /QOpenSys/usr/bin/true     # no extra thread
 *         ./exec_threads 1 /QOpenSys/usr/bin/true     # one extra thread
 *         ./exec_threads 4 /QOpenSys/usr/bin/true
 * On success the program is replaced and nothing more is printed here except
 * what <program> prints; the shell's $? is the program's status.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

extern char **environ;

static void *sleeper(void *arg) {
	(void)arg;
	for (;;)
		sleep(60);
	return NULL;
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: exec_threads nthreads program [args]\n");
		return 2;
	}
	int n = atoi(argv[1]);
	for (int i = 0; i < n; i++) {
		pthread_t t;
		if (pthread_create(&t, NULL, sleeper, NULL) != 0) {
			perror("pthread_create");
			return 2;
		}
	}
	usleep(100000); /* let the threads start */
	fprintf(stderr, "exec_threads: %d extra thread(s), calling execve(%s)\n", n, argv[2]);
	fflush(stderr);
	execve(argv[2], argv + 2, environ);
	fprintf(stderr, "exec_threads: execve returned: errno=%d (%s)\n", errno, strerror(errno));
	return 1;
}
