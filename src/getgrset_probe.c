/* getgrset_probe.c
 *
 * Does the string returned by getgrset on PASE have to be freed?  AIX documents
 * that it does ("This pointer must be freed by the user"); if PASE returned a
 * static buffer instead, free() would crash.  The Go code for os/user on IBM i
 * (internal/syscall/unix/getgrset_aix.go) calls free() on it, so check first.
 *
 * build:  /QOpenSys/pkgs/bin/gcc-12 -maix64 -o getgrset_probe getgrset_probe.c
 * run:    ./getgrset_probe user [iterations=200000 [mode]]
 *   mode 0 (default): call getgrset and free the result, every iteration
 *   mode 1:           call getgrset and never free the result
 *   mode 2:           call getgrset twice and compare the two pointers
 * Run the modes one after the other and compare the "growth" line: with mode 1
 * the growth is large if the result is allocated per call (so it must be freed)
 * and small if it is a static buffer.  Mode 0 must finish without crashing.
 * An unknown user name is also tried at the end.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <grp.h>
#include <dlfcn.h>

static char *(*gs)(const char *);

static long maxrss_kb(void) {
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	return (long)ru.ru_maxrss;
}

int main(int argc, char **argv) {
	const char *user = argc > 1 ? argv[1] : "jasond";
	long iterations = argc > 2 ? atol(argv[2]) : 200000;
	int mode = argc > 3 ? atoi(argv[3]) : 0;
	long i, before, after, nulls = 0;
	char *s, *t;

	{
		void *h = dlopen(NULL, RTLD_NOW);
		gs = h ? (char *(*)(const char *))dlsym(h, "getgrset") : NULL;
		if (gs == NULL) {
			printf("getgrset not found in libc\n");
			return 1;
		}
	}
	printf("user=%s iterations=%ld mode=%d\n", user, iterations, mode);
	s = gs(user);
	printf("first call: %s%s%s\n", s ? "\"" : "NULL", s ? s : "", s ? "\"" : "");

	if (mode == 2) {
		t = gs(user);
		printf("two calls: pointers %s (%p %p)\n", s == t ? "are the SAME (static buffer?)" : "differ (allocated per call)",
		       (void *)s, (void *)t);
		return 0;
	}

	before = maxrss_kb();
	for (i = 0; i < iterations; i++) {
		s = gs(user);
		if (s == NULL) {
			nulls++;
			continue;
		}
		if (mode == 0)
			free(s);
	}
	after = maxrss_kb();
	printf("%ld calls, %ld returned NULL, mode %d (%s the result)\n", iterations, nulls, mode,
	       mode == 0 ? "freed" : "did not free");
	printf("max RSS before=%ld after=%ld growth=%ld (units of getrusage ru_maxrss)\n", before, after, after - before);

	errno = 0;
	s = gs("nosuchuser_zz9");
	printf("unknown user: %s%s%s (errno %d: %s)\n", s ? "\"" : "NULL", s ? s : "", s ? "\"" : "", errno, strerror(errno));
	if (s != NULL && mode == 0)
		free(s);
	return 0;
}
