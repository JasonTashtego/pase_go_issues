/* if_probe.c
 *
 * What does IBM i PASE offer for listing network interfaces?  Go's net package
 * on aix calls getkerninfo(KINFO_RT_IFLIST) and then ioctl(SIOCGIFMTU) per
 * interface, and net.Interfaces() fails with EINVAL.  This tries those calls
 * and the alternatives (SIOCGIFCONF, if_nameindex, a getifaddrs symbol check),
 * and prints what each returns.  There is no <ifaddrs.h> on PASE.
 *
 * build:  /QOpenSys/pkgs/bin/gcc-12 -maix64 -O1 -o if_probe if_probe.c
 * run:    ./if_probe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

extern int getkerninfo(int op, char *where, int *size, int arg);

/* the value Go uses (net/interface_aix.go) */
#define GO_KINFO_RT_IFLIST ((0x1 << 8) | 3 | (1 << 30))

static void show(const char *label, int r) {
	if (r >= 0)
		printf("  ok        %s  (= %d)\n", label, r);
	else
		printf("  errno %-3d %-52s %s\n", errno, label, strerror(errno));
}

#define T(label, expr) do { errno = 0; int r_ = (expr); show(label, r_); } while (0)

static const char *addr2str(struct sockaddr *sa, char *buf, size_t n) {
	if (!sa) return "(none)";
	if (sa->sa_family == AF_INET)
		return inet_ntop(AF_INET, &((struct sockaddr_in *)sa)->sin_addr, buf, n);
	if (sa->sa_family == AF_INET6)
		return inet_ntop(AF_INET6, &((struct sockaddr_in6 *)sa)->sin6_addr, buf, n);
	snprintf(buf, n, "family %d", sa->sa_family);
	return buf;
}

static char names[32][IFNAMSIZ];
static int nnames;

static void addname(const char *n) {
	for (int i = 0; i < nnames; i++)
		if (!strcmp(names[i], n)) return;
	if (nnames < 32) {
		strncpy(names[nnames], n, IFNAMSIZ - 1);
		names[nnames][IFNAMSIZ - 1] = 0;
		nnames++;
	}
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0); /* unbuffered: a crash must not lose the output */
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) { perror("socket"); return 2; }

	printf("=== 1. what Go calls: getkerninfo(KINFO_RT_IFLIST = %#x)\n", GO_KINFO_RT_IFLIST);
	{
		int needed = 0;
		errno = 0;
		int r = getkerninfo(GO_KINFO_RT_IFLIST, NULL, &needed, 0);
		if (r < 0)
			printf("  errno %-3d getkerninfo(.., NULL, &needed, 0)  %s\n", errno, strerror(errno));
		else
			printf("  ok        getkerninfo(.., NULL, &needed, 0): returned %d, needed=%d\n", r, needed);
		printf("  trying getkerninfo(.., NULL, NULL, 0) [the form Go uses]; if the output stops here, this call crashed\n");
		errno = 0;
		r = getkerninfo(GO_KINFO_RT_IFLIST, NULL, NULL, 0);
		if (r < 0)
			printf("  errno %-3d getkerninfo(.., NULL, NULL, 0)  [the form Go uses]  %s\n", errno, strerror(errno));
		else
			printf("  ok        getkerninfo(.., NULL, NULL, 0) [the form Go uses]: returned %d\n", r);
		if (r > 0 || needed > 0) {
			int sz = (r > 0 ? r : needed);
			char *tab = malloc(sz + 64);
			int got = sz + 64;
			errno = 0;
			int r2 = getkerninfo(GO_KINFO_RT_IFLIST, tab, &got, 0);
			if (r2 < 0)
				printf("  errno %-3d getkerninfo(.., buf, &size, 0)  %s\n", errno, strerror(errno));
			else
				printf("  ok        getkerninfo(.., buf, &size, 0): returned %d, size=%d\n", r2, got);
			free(tab);
		}
	}

	printf("\n=== 2. does libc have getifaddrs at all?\n");
	{
		void *h = dlopen(NULL, RTLD_NOW);
		void *s = h ? dlsym(h, "getifaddrs") : NULL;
		printf("  getifaddrs symbol: %s\n", s ? "FOUND" : "not found");
		void *s2 = h ? dlsym(h, "if_nameindex") : NULL;
		printf("  if_nameindex symbol: %s\n", s2 ? "FOUND" : "not found");
	}

	printf("\n=== 3. ioctl(SIOCGIFCONF)\n");
	{
		char buf[8192];
		struct ifconf ifc;
		memset(&ifc, 0, sizeof ifc);
		ifc.ifc_len = sizeof buf;
		ifc.ifc_buf = buf;
		errno = 0;
		if (ioctl(sock, SIOCGIFCONF, &ifc) < 0) {
			printf("  errno %-3d ioctl(SIOCGIFCONF)  %s\n", errno, strerror(errno));
		} else {
			printf("  ok        SIOCGIFCONF, ifc_len=%d\n", ifc.ifc_len);
			char *q = buf;
			while (q < buf + ifc.ifc_len) {
				struct ifreq *r = (struct ifreq *)q;
				char a[64] = "";
				printf("    %-8s %s\n", r->ifr_name, addr2str(&r->ifr_addr, a, sizeof a));
				addname(r->ifr_name);
				size_t sl = r->ifr_addr.sa_len;
				size_t step = IFNAMSIZ + (sl > sizeof(struct sockaddr) ? sl : sizeof(struct sockaddr));
				q += step;
			}
		}
	}

	printf("\n=== 4. if_nameindex\n");
	{
		struct if_nameindex *ni = if_nameindex();
		if (!ni) {
			printf("  errno %-3d if_nameindex  %s\n", errno, strerror(errno));
		} else {
			printf("  ok        if_nameindex\n");
			for (struct if_nameindex *p = ni; p->if_index != 0; p++) {
				printf("    index %-3u %s\n", p->if_index, p->if_name);
				addname(p->if_name);
			}
			if_freenameindex(ni);
		}
	}

	printf("\n=== 5. per-interface ioctls\n");
	printf("    header values: SIOCGIFMTU=%#lx  SIOCGIFFLAGS=%#lx  SIOCGIFADDR=%#lx  SIOCGIFNETMASK=%#lx\n",
#ifdef SIOCGIFMTU
	       (unsigned long)SIOCGIFMTU,
#else
	       0UL,
#endif
	       (unsigned long)SIOCGIFFLAGS, (unsigned long)SIOCGIFADDR, (unsigned long)SIOCGIFNETMASK);
	printf("    Go's values:   SIOCGIFMTU=0xc02869aa  (-0x3fd796aa)\n");
	for (int i = 0; i < nnames; i++) {
		struct ifreq ifr;
		char label[96];
		printf("  interface %s\n", names[i]);

		memset(&ifr, 0, sizeof ifr);
		strncpy(ifr.ifr_name, names[i], IFNAMSIZ - 1);
		snprintf(label, sizeof label, "ioctl(SIOCGIFFLAGS, %s)", names[i]);
		errno = 0;
		if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0)
			show(label, -1);
		else
			printf("  ok        %s  flags=%#x\n", label, (unsigned)ifr.ifr_flags);

		memset(&ifr, 0, sizeof ifr);
		strncpy(ifr.ifr_name, names[i], IFNAMSIZ - 1);
		snprintf(label, sizeof label, "ioctl(SIOCGIFADDR, %s)", names[i]);
		errno = 0;
		if (ioctl(sock, SIOCGIFADDR, &ifr) < 0) {
			show(label, -1);
		} else {
			char a[64] = "";
			printf("  ok        %s  %s\n", label, addr2str(&ifr.ifr_addr, a, sizeof a));
		}

		memset(&ifr, 0, sizeof ifr);
		strncpy(ifr.ifr_name, names[i], IFNAMSIZ - 1);
		snprintf(label, sizeof label, "ioctl(SIOCGIFNETMASK, %s)", names[i]);
		errno = 0;
		if (ioctl(sock, SIOCGIFNETMASK, &ifr) < 0) {
			show(label, -1);
		} else {
			char a[64] = "";
			printf("  ok        %s  %s\n", label, addr2str(&ifr.ifr_addr, a, sizeof a));
		}

#ifdef SIOCGIFMTU
		memset(&ifr, 0, sizeof ifr);
		strncpy(ifr.ifr_name, names[i], IFNAMSIZ - 1);
		errno = 0;
		if (ioctl(sock, SIOCGIFMTU, &ifr) < 0)
			printf("  errno %-3d %-52s %s\n", errno, "ioctl(SIOCGIFMTU)", strerror(errno));
		else
			printf("  ok        ioctl(SIOCGIFMTU)   raw bytes: %02x %02x %02x %02x\n",
			       (unsigned char)ifr.ifr_ifru.ifru_data[0], (unsigned char)ifr.ifr_ifru.ifru_data[1],
			       (unsigned char)ifr.ifr_ifru.ifru_data[2], (unsigned char)ifr.ifr_ifru.ifru_data[3]);
#endif
	}
	return 0;
}
