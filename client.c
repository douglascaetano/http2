/**
 * HTTP/2 minimalistic client
 */

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <event2/event.h>

#include "defines.h"
#include "util.h"

#include "client.h"

/* libevent's structures */
struct event_base *evbase;
struct event *evsock;

static void usage(void);

int
main(int argc, char *argv[])
{
	int sockfd;
	int r;
	char *host;
	char *port = SERVER_PORT_DEFAULT;
	char ch;

	/* Parse arguments */
	while ((ch = getopt(argc, argv, "hp:")) != -1) {
		switch (ch) {
		case 'p':
			port = optarg;
			break;
		case 'h':
		default:
			usage();
		}
	}
	host = argv[optind];
	if (host == NULL) {
		prterr("missing host to connect to.");
		usage();
	}

	printf("HTTP/2 client\n");

	/* Opens the socket */
	sockfd = client_connect(host, port);
	if (sockfd < 0) {
		prterr("client_connect: failure\n");
		exit(1);
	}

	/* Creates libevent event_base struct */
	evbase = event_base_new();
	if (evbase == NULL) {
		close(sockfd);
		prterr("event_base_new: failure\n");
		exit(1);
	}

	/* Creates an event notification for reading on the socket */
	evsock = event_new(evbase, sockfd, EV_READ, client_read, NULL);
	if (evsock == NULL) {
		close(sockfd);
		event_base_free(evbase);
		prterr("event_new: failure\n");
		exit(1);
	}
	if (event_add(evsock, NULL) < 0) {
		close(sockfd);
		event_free(evsock);
		event_base_free(evbase);
		prterr("event_add: failure\n");
		exit(1);
	}

	/* Dispatch events */
	r = event_base_dispatch(evbase);
	if (r < 0) {
		close(sockfd);
		event_free(evsock);
		event_base_free(evbase);
		prterr("event_base_dispatch: failure\n");
		exit(1);
	}

	event_free(evsock);
	event_base_free(evbase);
	return 0;
}

void
client_read(evutil_socket_t fd, short events, void *arg)
{
	char buf[100];
	size_t len;
	ssize_t s;

	/* Reads received data */
	len = sizeof(buf);
	s = recv(fd, buf, len, 0);
	if (s < 0)
		perror("recv");
	else if (s == 0)
		/* connection was closed */
		return;

	/* Prints received information */
	printf("%s", buf);

	/* Rearms event */
	event_add(evsock, NULL);
}

int
client_connect(char *host, char *port)
{
	struct sockaddr_in *sin;
	struct addrinfo h, *ai;
	char ip[INET_ADDRSTRLEN];
	int e;
	int fd;

	/* Gets local address */
	memset(&h, 0, sizeof(h));
	h.ai_family = AF_INET;
	h.ai_socktype = SOCK_STREAM;
	e = getaddrinfo(host, port, &h, &ai);
	if (e) {
		prterr("getaddrinfo: %s\n", gai_strerror(e));
		exit(1);
	}
	if (ai == NULL) {
		prterr("getaddrinfo: no address found.\n");
		exit(1);
	}

	/* Opens the socket and connect to the host */
	fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if (fd < 0) {
		perror("socket");
		exit(1);
	}
	if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
		perror("connect");
		exit(1);
	}

	sin = (struct sockaddr_in *)ai->ai_addr;
	inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
	printf("connected to %s:%d...\n", ip, ntohs(sin->sin_port));

	return fd;
}

static void
usage(void)
{
	fprintf(stderr, "usage: %s [-p port] host\n", __progname);
	exit(1);
}

