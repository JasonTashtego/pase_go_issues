/* if_probe2.c
 *
 * Follow-up to if_probe.c.  getkerninfo(KINFO_RT_IFLIST) works; net.Interfaces()
 * still fails with EINVAL.  Hypothesis: Go takes each interface's name from the
 * routing message (a line name such as ETHLINE1) and passes it to
 * ioctl(SIOCGIFMTU), which only accepts the IP-address name that SIOCGIFCONF
 * uses (10.150.0.37).  This decodes the kernel table the way Go does, then
 * tries the per-interface ioctls with both kinds of name.
 *
 * build:  /QOpenSys/pkgs/bin/gcc-12 -maix64 -O1 -o if_probe2 if_probe2.c
 * run:    ./if_probe2
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

extern int getkerninfo(int op, char *where, int *size, int arg);

#define KINFO_RT_IFLIST ((0x1 << 8) | 3 | (1 << 30))

/* Go's layouts (syscall/ztypes_aix_ppc64.go, net/interface_aix.go) */
struct ifmsg {
	unsigned short msglen;
	unsigned char version, type;
	int addrs, flags;
	unsigned short index;
	unsigned char addrlen, pad;
};
#define IFMSG_SIZE 0x10
#define RTM_IFINFO_ 0xe
#define RTM_NEWADDR_ 0xc
struct sdl {
	unsigned char len, family;
	unsigned short index;
	unsigned char type, nlen, alen, slen;
	unsigned char data[120];
};

static int sock;

static const char *ip(struct sockaddr *sa, char *b, size_t n) {
	if (sa->sa_family == AF_INET)
		return inet_ntop(AF_INET, &((struct sockaddr_in *)sa)->sin_addr, b, n);
	if (sa->sa_family == AF_INET6)
		return inet_ntop(AF_INET6, &((struct sockaddr_in6 *)sa)->sin6_addr, b, n);
	snprintf(b, n, "family %d", sa->sa_family);
	return b;
}

static void tryname(const char *kind, const char *name) {
	struct ifreq ifr;
	printf("    [%s name \"%s\"]\n", kind, name);
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	errno = 0;
	if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0)
		printf("        SIOCGIFFLAGS: errno %d (%s)\n", errno, strerror(errno));
	else
		printf("        SIOCGIFFLAGS: ok, flags=%#x\n", (unsigned)ifr.ifr_flags);

	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	errno = 0;
	if (ioctl(sock, SIOCGIFMTU, &ifr) < 0) {
		printf("        SIOCGIFMTU:   errno %d (%s)\n", errno, strerror(errno));
	} else {
		unsigned char *u = (unsigned char *)&ifr.ifr_ifru;
		int asint;
		memcpy(&asint, u, sizeof asint);
		printf("        SIOCGIFMTU:   ok, first 4 bytes of the union %02x %02x %02x %02x = %d (as a native int)\n",
		       u[0], u[1], u[2], u[3], asint);
	}

	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	errno = 0;
	if (ioctl(sock, SIOCGIFADDR, &ifr) < 0) {
		printf("        SIOCGIFADDR:  errno %d (%s)\n", errno, strerror(errno));
	} else {
		char b[64];
		printf("        SIOCGIFADDR:  ok, %s\n", ip(&ifr.ifr_addr, b, sizeof b));
	}
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) { perror("socket"); return 2; }

	int needed = getkerninfo(KINFO_RT_IFLIST, NULL, NULL, 0);
	printf("getkerninfo size query (Go's form) returned %d\n", needed);
	if (needed <= 0) return 1;
	char *tab = malloc(needed + 64);
	int size = needed + 64;
	int r = getkerninfo(KINFO_RT_IFLIST, tab, &size, 0);
	printf("getkerninfo read returned %d, size=%d\n\n", r, size);
	if (r < 0) return 1;

	printf("=== the table, decoded the way Go decodes it\n");
	static char linknames[16][64];
	int nlink = 0;
	char *p = tab;
	int left = size;
	while (left > 0) {
		struct ifmsg *m = (struct ifmsg *)p;
		if (m->msglen == 0) break;
		if (m->type == RTM_IFINFO_) {
			struct sdl *s = (struct sdl *)(p + IFMSG_SIZE);
			char name[64] = "";
			unsigned n = s->nlen < 63 ? s->nlen : 63;
			memcpy(name, s->data, n);
			printf("  IFINFO   msglen=%-3u index=%-2u flags=%#-8x  link name=\"%s\" (nlen=%u alen=%u)\n",
			       m->msglen, m->index, (unsigned)m->flags, name, s->nlen, s->alen);
			if (nlink < 16) strcpy(linknames[nlink++], name);
		} else if (m->type == RTM_NEWADDR_) {
			unsigned mask = (unsigned)m->addrs;
			unsigned off = IFMSG_SIZE;
			struct sockaddr *iprsa = NULL, *nmrsa = NULL;
			for (unsigned i = 0; i < 8; i++) {
				if (!(mask & (1u << i))) continue;
				struct sockaddr *rsa = (struct sockaddr *)(p + off);
				if (i == 2) nmrsa = rsa;
				if (i == 5) iprsa = rsa;
				off += (rsa->sa_len + 3) & ~3u;
			}
			char a[64] = "(none)", nm[64] = "(none)";
			if (iprsa) ip(iprsa, a, sizeof a);
			if (nmrsa) ip(nmrsa, nm, sizeof nm);
			printf("  NEWADDR  msglen=%-3u index=%-2u addrs=%#-4x  addr=%s netmask=%s\n", m->msglen, m->index, mask, a, nm);
		} else {
			printf("  type %-3u msglen=%u index=%u\n", m->type, m->msglen, m->index);
		}
		p += m->msglen;
		left -= m->msglen;
	}

	printf("\n=== ioctls with each kind of name\n");
	for (int i = 0; i < nlink; i++) {
		printf("  interface from the routing table: link name \"%s\"\n", linknames[i]);
		tryname("link", linknames[i]);
	}
	{
		char buf[4096];
		struct ifconf ifc;
		memset(&ifc, 0, sizeof ifc);
		ifc.ifc_len = sizeof buf;
		ifc.ifc_buf = buf;
		if (ioctl(sock, SIOCGIFCONF, &ifc) == 0) {
			char *q = buf;
			while (q < buf + ifc.ifc_len) {
				struct ifreq *rr = (struct ifreq *)q;
				printf("  interface from SIOCGIFCONF: ip name \"%s\"\n", rr->ifr_name);
				tryname("ip", rr->ifr_name);
				size_t sl = rr->ifr_addr.sa_len;
				q += IFNAMSIZ + (sl > sizeof(struct sockaddr) ? sl : sizeof(struct sockaddr));
			}
		}
	}
	return 0;
}
