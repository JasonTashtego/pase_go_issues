/* group_probe.c
 *
 * What can PASE tell a program about users and groups?  Go's os/user on aix
 * reads /etc/group and /etc/passwd (listgroups_unix.go has "|| aix"), and those
 * files do not exist here.  This prints:
 *   - which of the usual user and group files exist
 *   - the ids of this process
 *   - getpwuid, getpwnam, getgrgid, getgrnam, getgroups results
 *   - which of getgrset, getgrouplist, setgrent, getgrent, endgrent exist in
 *     libc (looked up with dlsym, so a missing one does not stop the build)
 *     and what they return, including a walk of the whole group database
 *
 * build:  /QOpenSys/pkgs/bin/gcc-12 -maix64 -o group_probe group_probe.c
 *         (if the link fails on dlopen, add -ldl)
 * run:    ./group_probe [user]
 * The default user is the one running the program.  Run it once as each profile
 * of interest (for example the *ALLOBJ one and a plain one).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/stat.h>

static void show_group(const char *how, struct group *g) {
	if (g == NULL) {
		printf("  %s: NULL (errno %d: %s)\n", how, errno, strerror(errno));
		return;
	}
	printf("  %s: name=%s gid=%ld members:", how, g->gr_name, (long)g->gr_gid);
	if (g->gr_mem != NULL) {
		char **m;
		for (m = g->gr_mem; *m != NULL; m++)
			printf(" %s", *m);
	}
	printf("\n");
}

static void show_file(const char *path) {
	struct stat st;
	if (stat(path, &st) == 0)
		printf("  %-34s exists, %ld bytes\n", path, (long)st.st_size);
	else
		printf("  %-34s no (%s)\n", path, strerror(errno));
}

int main(int argc, char **argv) {
	static const char *files[] = {
		"/etc/passwd", "/etc/group",
		"/QOpenSys/etc/passwd", "/QOpenSys/etc/group",
		"/etc/security/passwd", "/etc/security/group",
		"/QOpenSys/etc/security/passwd", "/QOpenSys/etc/security/group",
		NULL
	};
	int i;
	struct passwd *pw;
	struct group *gr;
	const char *user;
	void *h;

	printf("files\n");
	for (i = 0; files[i] != NULL; i++)
		show_file(files[i]);

	printf("ids: uid=%ld euid=%ld gid=%ld egid=%ld\n",
	       (long)getuid(), (long)geteuid(), (long)getgid(), (long)getegid());

	errno = 0;
	pw = getpwuid(getuid());
	if (pw == NULL) {
		printf("getpwuid(%ld): NULL (errno %d: %s)\n", (long)getuid(), errno, strerror(errno));
		return 1;
	}
	printf("getpwuid: name=%s uid=%ld gid=%ld dir=%s shell=%s\n",
	       pw->pw_name, (long)pw->pw_uid, (long)pw->pw_gid,
	       pw->pw_dir ? pw->pw_dir : "(null)", pw->pw_shell ? pw->pw_shell : "(null)");

	user = argc > 1 ? argv[1] : pw->pw_name;
	errno = 0;
	pw = getpwnam(user);
	if (pw == NULL) {
		printf("getpwnam(%s): NULL (errno %d: %s)\n", user, errno, strerror(errno));
		return 1;
	}
	printf("getpwnam(%s): uid=%ld gid=%ld dir=%s\n", user, (long)pw->pw_uid, (long)pw->pw_gid,
	       pw->pw_dir ? pw->pw_dir : "(null)");

	{
		long gid = (long)pw->pw_gid;
		printf("groups of %s by gid and by name\n", user);
		errno = 0;
		gr = getgrgid((gid_t)gid);
		show_group("getgrgid(primary)", gr);
		if (gr != NULL) {
			char name[256];
			strncpy(name, gr->gr_name, sizeof name - 1);
			name[sizeof name - 1] = 0;
			errno = 0;
			show_group("getgrnam(primary name)", getgrnam(name));
		}
	}

	{
		int n = getgroups(0, NULL);
		printf("getgroups: count=%d\n", n);
		if (n > 0) {
			gid_t *list = calloc((size_t)n, sizeof *list);
			int got = getgroups(n, list);
			printf("  got %d:", got);
			for (i = 0; i < got; i++)
				printf(" %ld", (long)list[i]);
			printf("\n");
			for (i = 0; i < got; i++) {
				errno = 0;
				show_group("getgrgid", getgrgid(list[i]));
			}
			free(list);
		}
	}

	h = dlopen(NULL, RTLD_NOW);
	if (h == NULL) {
		printf("dlopen(NULL) failed: %s\n", dlerror());
		return 1;
	}
	{
		void *p_getgrset = dlsym(h, "getgrset");
		void *p_getgrouplist = dlsym(h, "getgrouplist");
		void *p_setgrent = dlsym(h, "setgrent");
		void *p_getgrent = dlsym(h, "getgrent");
		void *p_endgrent = dlsym(h, "endgrent");
		printf("libc has: getgrset=%s getgrouplist=%s setgrent=%s getgrent=%s endgrent=%s\n",
		       p_getgrset ? "yes" : "no", p_getgrouplist ? "yes" : "no",
		       p_setgrent ? "yes" : "no", p_getgrent ? "yes" : "no", p_endgrent ? "yes" : "no");

		if (p_getgrset != NULL) {
			char *(*fn)(const char *) = (char *(*)(const char *))p_getgrset;
			char *s;
			errno = 0;
			s = fn(user);
			if (s == NULL)
				printf("getgrset(%s): NULL (errno %d: %s)\n", user, errno, strerror(errno));
			else
				printf("getgrset(%s): \"%s\"\n", user, s);
		}

		if (p_getgrouplist != NULL) {
			int (*fn)(const char *, gid_t, gid_t *, int *) =
				(int (*)(const char *, gid_t, gid_t *, int *))p_getgrouplist;
			gid_t ids[256];
			int n = 256, rc;
			errno = 0;
			rc = fn(user, pw->pw_gid, ids, &n);
			printf("getgrouplist(%s): rc=%d n=%d (errno %d):", user, rc, n, errno);
			for (i = 0; i < n && i < 256; i++)
				printf(" %ld", (long)ids[i]);
			printf("\n");
		}

		if (p_setgrent != NULL && p_getgrent != NULL) {
			void (*set)(void) = (void (*)(void))p_setgrent;
			struct group *(*get)(void) = (struct group *(*)(void))p_getgrent;
			void (*end)(void) = (void (*)(void))p_endgrent;
			int count = 0, listed = 0, found = 0;
			set();
			errno = 0;
			while ((gr = get()) != NULL && count < 100000) {
				char **m;
				int is_member = 0;
				count++;
				if (gr->gr_mem != NULL)
					for (m = gr->gr_mem; *m != NULL; m++)
						if (strcmp(*m, user) == 0)
							is_member = 1;
				if (is_member) {
					found++;
					show_group("member of", gr);
				} else if (listed < 5) {
					listed++;
					show_group("sample", gr);
				}
			}
			if (end != NULL)
				end();
			printf("getgrent walk: %d groups in the database, %s is listed as a member of %d\n",
			       count, user, found);
			if (count == 0)
				printf("  (the walk returned nothing; errno %d: %s)\n", errno, strerror(errno));
		}
	}
	return 0;
}
