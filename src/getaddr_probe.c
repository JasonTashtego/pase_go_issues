/* getaddr_probe.c
 *
 * getservbyname("☺") fails on PASE (getserv_probe), yet Go's port lookup for
 * "☺" succeeds there.  Go's cgo resolver does getaddrinfo(NULL, service, hints)
 * with ai_flags = 0, ai_socktype and ai_protocol for tcp or udp,
 * ai_family = AF_UNSPEC (net/cgo_unix.go cgoLookupPort).  Run exactly that for a few service names and print
 * the return code and every address returned.
 *
 * build:  /QOpenSys/pkgs/bin/gcc-12 -maix64 -o getaddr_probe getaddr_probe.c
 * run:    ./getaddr_probe
 */
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

static void try(const char *service, int socktype, const char *label) {
	struct addrinfo hints, *res = NULL, *r;
	int rc;
	memset(&hints, 0, sizeof hints);
	hints.ai_flags = 0;  /* cgoLookupPort sets no flags */
	hints.ai_protocol = socktype == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
	hints.ai_socktype = socktype;
	hints.ai_family = AF_UNSPEC;
	rc = getaddrinfo(NULL, service, &hints, &res);
	if (rc != 0) {
		printf("getaddrinfo(NULL, \"%s\", %s): rc=%d (%s)\n", service, label, rc, gai_strerror(rc));
		return;
	}
	printf("getaddrinfo(NULL, \"%s\", %s): rc=0\n", service, label);
	for (r = res; r != NULL; r = r->ai_next) {
		int port = -1;
		if (r->ai_family == AF_INET)
			port = ntohs(((struct sockaddr_in *)r->ai_addr)->sin_port);
		else if (r->ai_family == AF_INET6)
			port = ntohs(((struct sockaddr_in6 *)r->ai_addr)->sin6_port);
		printf("    family=%d socktype=%d port=%d\n", r->ai_family, r->ai_socktype, port);
	}
	freeaddrinfo(res);
}

int main(void) {
	static const char *names[] = { "\xe2\x98\xba", "nosuchservice", "http", "80", "-1", "123456789", NULL };
	int i;
	for (i = 0; names[i] != NULL; i++) {
		try(names[i], SOCK_STREAM, "tcp");
		try(names[i], SOCK_DGRAM, "udp");
	}
	return 0;
}
