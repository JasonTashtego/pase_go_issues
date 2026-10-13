/* at_probe.c
 *
 * Which of the *at system calls work on IBM i PASE with a real directory
 * descriptor, and with which flag values?  Go's os.Root uses all of them, and
 * every one fails with EINVAL.  This prints the flag values from the system
 * headers next to the values Go uses on aix, then runs each call with both.
 *
 * build:  /QOpenSys/pkgs/bin/gcc-12 -maix64 -O1 -o at_probe at_probe.c
 * run:    ./at_probe
 */
#define _ATFILE_SOURCE 1
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

/* the values Go uses on aix (internal/syscall/unix/at_aix.go, syscall/zerrors_aix_ppc64.go) */
#define GO_AT_FDCWD (-2)
#define GO_AT_REMOVEDIR 0x1
#define GO_AT_SYMLINK_NOFOLLOW 0x1
#define GO_O_NOFOLLOW 0x1000000
#define GO_O_DIRECTORY 0x80000
#define GO_O_CLOEXEC 0x800000

static char dir[256];

static void show(const char *label, int r) {
	if (r >= 0)
		printf("  ok        %s\n", label);
	else
		printf("  errno %-3d %-48s %s\n", errno, label, strerror(errno));
}

#define T(label, expr) do { errno = 0; int r_ = (expr); show(label, r_); } while (0)

static void setup(void) {
	char cmd[512];
	snprintf(cmd, sizeof cmd, "rm -rf %s; mkdir %s && echo data > %s/f && ln -s f %s/l && mkdir %s/d", dir, dir, dir, dir, dir);
	if (system(cmd) != 0) { fprintf(stderr, "setup failed\n"); exit(2); }
}

static void run(const char *kind, int dfd) {
	struct stat st;
	char buf[64];
	printf("\n=== directory descriptor opened with %s (fd %d)\n", kind, dfd);

	printf("openat:\n");
	T("openat(dfd, \"f\", O_RDONLY)", openat(dfd, "f", O_RDONLY));
#ifdef O_NOFOLLOW
	T("openat(dfd, \"f\", O_RDONLY|O_NOFOLLOW [header])", openat(dfd, "f", O_RDONLY | O_NOFOLLOW));
	T("openat(dfd, \"l\", O_RDONLY|O_NOFOLLOW [header]) (a symlink; want ELOOP)", openat(dfd, "l", O_RDONLY | O_NOFOLLOW));
#endif
	T("openat(dfd, \"f\", O_RDONLY|0x1000000 [Go's O_NOFOLLOW])", openat(dfd, "f", O_RDONLY | GO_O_NOFOLLOW));
#ifdef O_CLOEXEC
	T("openat(dfd, \"f\", O_RDONLY|O_CLOEXEC [header])", openat(dfd, "f", O_RDONLY | O_CLOEXEC));
#endif
	T("openat(dfd, \"f\", O_RDONLY|0x800000 [Go's O_CLOEXEC])", openat(dfd, "f", O_RDONLY | GO_O_CLOEXEC));
#ifdef O_DIRECTORY
	T("openat(dfd, \"d\", O_RDONLY|O_DIRECTORY [header])", openat(dfd, "d", O_RDONLY | O_DIRECTORY));
#endif
	T("openat(dfd, \"d\", O_RDONLY|0x80000 [Go's O_DIRECTORY])", openat(dfd, "d", O_RDONLY | GO_O_DIRECTORY));
	T("openat(dfd, \"d\", O_RDONLY)", openat(dfd, "d", O_RDONLY));
	T("openat(dfd, \"new\", O_WRONLY|O_CREAT|O_EXCL, 0600)", openat(dfd, "new", O_WRONLY | O_CREAT | O_EXCL, 0600));
	T("openat(dfd, \"new2\", O_WRONLY|O_CREAT|O_NOFOLLOW(Go)|O_CLOEXEC(Go), 0600)", openat(dfd, "new2", O_WRONLY | O_CREAT | GO_O_NOFOLLOW | GO_O_CLOEXEC, 0600));
	T("openat(AT_FDCWD [header], <dir>/f, O_RDONLY)", ({ char p[300]; snprintf(p, sizeof p, "%s/f", dir); openat(AT_FDCWD, p, O_RDONLY); }));
	T("openat(-2 [Go's AT_FDCWD], <dir>/f, O_RDONLY)", ({ char p[300]; snprintf(p, sizeof p, "%s/f", dir); openat(GO_AT_FDCWD, p, O_RDONLY); }));

	printf("fstatat:\n");
	T("fstatat(dfd, \"f\", flags 0)", fstatat(dfd, "f", &st, 0));
	T("fstatat(dfd, \"l\", AT_SYMLINK_NOFOLLOW [header])", fstatat(dfd, "l", &st, AT_SYMLINK_NOFOLLOW));
	T("fstatat(dfd, \"l\", 0x1 [Go's AT_SYMLINK_NOFOLLOW])", fstatat(dfd, "l", &st, GO_AT_SYMLINK_NOFOLLOW));

	printf("mkdirat / unlinkat / renameat:\n");
	T("mkdirat(dfd, \"d2\", 0700)", mkdirat(dfd, "d2", 0700));
	T("unlinkat(dfd, \"d2\", AT_REMOVEDIR [header])", unlinkat(dfd, "d2", AT_REMOVEDIR));
	T("mkdirat(dfd, \"d3\", 0700)", mkdirat(dfd, "d3", 0700));
	T("unlinkat(dfd, \"d3\", 0x1 [Go's AT_REMOVEDIR])", unlinkat(dfd, "d3", GO_AT_REMOVEDIR));
	T("mkdirat(dfd, \"d4\", 0700)", mkdirat(dfd, "d4", 0700));
	T("unlinkat(dfd, \"d4\", 0x2)", unlinkat(dfd, "d4", 0x2));
	T("unlinkat(dfd, \"d4\", 0x4)", unlinkat(dfd, "d4", 0x4));
	T("unlinkat(dfd, \"d4\", 0x8)", unlinkat(dfd, "d4", 0x8));
	T("unlinkat(dfd, \"new\", 0)  (a file)", unlinkat(dfd, "new", 0));
	T("unlinkat(dfd, \"new2\", 0) (a file)", unlinkat(dfd, "new2", 0));
	T("unlinkat(dfd, \"d4\", AT_REMOVEDIR [header]) (if still there)", unlinkat(dfd, "d4", AT_REMOVEDIR));
	T("renameat(dfd, \"f\", dfd, \"f2\")", renameat(dfd, "f", dfd, "f2"));
	T("renameat(dfd, \"f2\", dfd, \"f\")", renameat(dfd, "f2", dfd, "f"));

	printf("symlinkat / readlinkat / linkat:\n");
	T("symlinkat(\"f\", dfd, \"l2\")", symlinkat("f", dfd, "l2"));
	T("readlinkat(dfd, \"l\", buf)", (int)readlinkat(dfd, "l", buf, sizeof buf));
	T("linkat(dfd, \"f\", dfd, \"h\", 0)", linkat(dfd, "f", dfd, "h", 0));
	unlinkat(dfd, "l2", 0);
	unlinkat(dfd, "h", 0);

	printf("fchmodat / fchownat:\n");
	T("fchmodat(dfd, \"f\", 0600, 0)", fchmodat(dfd, "f", 0600, 0));
	T("fchmodat(dfd, \"f\", 0600, AT_SYMLINK_NOFOLLOW [header])", fchmodat(dfd, "f", 0600, AT_SYMLINK_NOFOLLOW));
	T("fchownat(dfd, \"f\", -1, -1, 0)", fchownat(dfd, "f", (uid_t)-1, (gid_t)-1, 0));
}

int main(void) {
	printf("header values:  AT_FDCWD=%d  AT_SYMLINK_NOFOLLOW=%#x  AT_REMOVEDIR=%#x  AT_EACCESS=%#x\n",
	       (int)AT_FDCWD, (unsigned)AT_SYMLINK_NOFOLLOW, (unsigned)AT_REMOVEDIR,
#ifdef AT_EACCESS
	       (unsigned)AT_EACCESS
#else
	       0xdead
#endif
	);
	printf("                O_NOFOLLOW=%s  O_DIRECTORY=%s  O_CLOEXEC=%s\n",
#ifdef O_NOFOLLOW
	       "defined",
#else
	       "NOT defined",
#endif
#ifdef O_DIRECTORY
	       "defined",
#else
	       "NOT defined",
#endif
#ifdef O_CLOEXEC
	       "defined"
#else
	       "NOT defined"
#endif
	);
#ifdef O_NOFOLLOW
	printf("                O_NOFOLLOW=%#x\n", (unsigned)O_NOFOLLOW);
#endif
#ifdef O_DIRECTORY
	printf("                O_DIRECTORY=%#x\n", (unsigned)O_DIRECTORY);
#endif
#ifdef O_CLOEXEC
	printf("                O_CLOEXEC=%#x\n", (unsigned)O_CLOEXEC);
#endif
	printf("Go's aix values: AT_FDCWD=-2  AT_SYMLINK_NOFOLLOW=0x1  AT_REMOVEDIR=0x1  O_NOFOLLOW=0x1000000  O_DIRECTORY=0x80000  O_CLOEXEC=0x800000\n");

	snprintf(dir, sizeof dir, "/tmp/atprobe.%d", (int)getpid());
	setup();

	int fd1 = open(dir, O_RDONLY);
	if (fd1 < 0) { perror("open dir"); return 2; }
	run("open(dir, O_RDONLY)", fd1);
	close(fd1);
	setup(); /* start each run from the same state */

#ifdef O_DIRECTORY
	int fd2 = open(dir, O_RDONLY | O_DIRECTORY);
	if (fd2 >= 0) {
		run("open(dir, O_RDONLY|O_DIRECTORY)", fd2);
		close(fd2);
	} else {
		printf("\nopen(dir, O_RDONLY|O_DIRECTORY) failed: %s\n", strerror(errno));
	}
	setup();
#endif

	DIR *d = opendir(dir);
	if (d) {
		run("dirfd(opendir(dir))", dirfd(d));
		closedir(d);
	}
	{
		char cmd[300];
		snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
		(void)system(cmd);
	}
	return 0;
}
