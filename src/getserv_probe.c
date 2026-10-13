/* getserv_probe.c
 *
 * Why do net TestDialError/4, TestListenError/tcp_127.0.0.1:☺ and
 * TestListenPacketError/udp_127.0.0.1:☺ succeed on PASE?  The tests expect
 * port "☺" (UTF-8 e2 98 ba) to be rejected as an unknown service.  Go's
 * LookupPort on aix goes through getservbyname when cgo is on.  This asks libc
 * directly, for "☺" and for a few controls.
 *
 * build:  /QOpenSys/pkgs/bin/gcc-12 -maix64 -o getserv_probe getserv_probe.c
 * run:    ./getserv_probe
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <arpa/inet.h>

static void try(const char *name, const char *proto) {
	struct servent *se;
	errno = 0;
	se = getservbyname(name, proto);
	if (se == NULL)
		printf("getservbyname(\"%s\", %s): NULL (errno %d)\n", name, proto, errno);
	else
		printf("getservbyname(\"%s\", %s): s_name=\"%s\" port=%d proto=%s\n", name, proto,
		       se->s_name, ntohs((unsigned short)se->s_port), se->s_proto);
}

int main(void) {
	static const char *names[] = { "\xe2\x98\xba", "nosuchservice", "http", "ftp", "telnet", "", NULL };
	int i;
	for (i = 0; names[i] != NULL; i++) {
		try(names[i], "tcp");
		try(names[i], "udp");
	}
	return 0;
}
