/* sem_fork_probe.c
 *
 * sizeof(sem_t) is 4, so the Go runtime's 4-byte malloc is fine.  Does
 * sem_init fail (EINVAL) while another thread is creating child processes?
 * The Go failure (all15: cmd/go running "go list" = forks in other threads)
 * happened in a busy, forking process.
 *
 * build:  /QOpenSys/pkgs/bin/gcc-12 -maix64 -o sem_fork_probe sem_fork_probe.c -lpthread
 * run:    ./sem_fork_probe [count=20000 [semthreads=4 [spawners=2 [nomalloc=0]]]]
 *   semthreads  threads calling sem_init (destroy each right away)
 *   nomalloc    1 = sem_init into one preallocated array, no malloc/free/destroy in sem threads
*   spawners    threads running posix_spawn of /QOpenSys/usr/bin/true + waitpid
 * Run with spawners=0 as the control, then 2, interleaved, several times.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;
static long count;
static volatile int stop;
static volatile long fails;
static int nomalloc;
static sem_t *pool;

static void *semworker(void *arg) {
	long i;
	for (i = 0; i < count && !fails; i++) {
		sem_t *s = nomalloc ? &pool[(long)arg * count + i] : (sem_t *)malloc(sizeof *s);
		errno = 0;
		if (sem_init(s, 0, 0) != 0) {
			printf("sem thread %ld: sem_init FAILED at %ld: errno %d (%s)\n", (long)arg, i, errno, strerror(errno));
			fails++;
			return NULL;
		}
		if (!nomalloc) {
			sem_destroy(s);
			free(s);
		}
	}
	return NULL;
}

static void *spawner(void *arg) {
	char *av[] = {"true", NULL};
	long n = 0;
	while (!stop) {
		pid_t pid;
		int st;
		if (posix_spawn(&pid, "/QOpenSys/usr/bin/true", NULL, NULL, av, environ) == 0)
			waitpid(pid, &st, 0);
		n++;
	}
	printf("spawner %ld: %ld spawns\n", (long)arg, n);
	return NULL;
}

int main(int argc, char **argv) {
	int st = argc > 2 ? atoi(argv[2]) : 4, sp = argc > 3 ? atoi(argv[3]) : 2, i;
	pthread_t t[64], p[64];
	count = argc > 1 ? atol(argv[1]) : 20000;
	nomalloc = argc > 4 ? atoi(argv[4]) : 0;
	if (nomalloc) pool = calloc((size_t)st * count, sizeof(sem_t));
	printf("count=%ld semthreads=%d spawners=%d\n", count, st, sp);
	for (i = 0; i < sp; i++) pthread_create(&p[i], NULL, spawner, (void *)(long)i);
	for (i = 0; i < st; i++) pthread_create(&t[i], NULL, semworker, (void *)(long)i);
	for (i = 0; i < st; i++) pthread_join(t[i], NULL);
	stop = 1;
	for (i = 0; i < sp; i++) pthread_join(p[i], NULL);
	printf("%s\n", fails ? "FAILED" : "no failure");
	return fails != 0;
}
