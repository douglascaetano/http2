/**
 * HTTP/2 minimalistic server
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
#include "http2.h"
#include "util.h"

#include "server.h"

/* libevent's structures */
struct event_base *evbase;
struct event *evsock;

static void usage(void);

int
main(int argc, char *argv[])
{
	int sockfd;
	int r;
	char *server_port = SERVER_PORT_DEFAULT;
	char ch;

	/* Parse arguments */
	while ((ch = getopt(argc, argv, "hp:a:")) != -1) {
		switch (ch) {
		case 'p':
			server_port = optarg;
			break;
		case 'h':
		default:
			usage();
		}
	}

	printf("HTTP/2 server\n");

	/* Opens the listening socket */
	sockfd = server_listen(server_port);
	if (sockfd < 0) {
		prterr("server_listen: failure.");
		exit(1);
	}

	/* Creates libevent event_base struct */
	evbase = event_base_new();
	if (evbase == NULL) {
		close(sockfd);
		prterr("event_base_new: failure.");
		exit(1);
	}

	/* Creates an event notification for the listening socket */
	evsock = event_new(evbase, sockfd, EV_READ, server_accept, NULL);
	if (evsock == NULL) {
		close(sockfd);
		event_base_free(evbase);
		prterr("event_new: failure.");
		exit(1);
	}
	if (event_add(evsock, NULL) < 0) {
		close(sockfd);
		event_base_free(evbase);
		prterr("event_add: failure.");
		exit(1);
	}

	/* Dispatch events */
	r = event_base_dispatch(evbase);
	if (r < 0)
		prterr("event_base_dispatch: failure.");

	close(sockfd);
	event_base_free(evbase);
	return 0;
}

void
server_accept(evutil_socket_t fd, short events, void *arg)
{
	char ip[INET_ADDRSTRLEN];
	struct sockaddr_in addr;
	socklen_t addrlen;
	int connfd;

	/* Rearms the listening socket's event */
	event_add(evsock, NULL);

	/* Accepts incomming connection */
	addrlen = sizeof(addr);
	connfd = accept(fd, (struct sockaddr *)&addr, &addrlen);
	if (connfd < 0) {
		prterr("accept: failed to accept an incoming connection.");
		return;
	}

	/* Prints information on accepted connection */
	inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
	printf("(%d) new connection received from %s:%d\n",
	    connfd, ip, ntohs(addr.sin_port));

	http2_connection_new(connfd, evbase);
}

int
server_listen(char *port)
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
	h.ai_flags = AI_PASSIVE;
	e = getaddrinfo(NULL, port, &h, &ai);
	if (e) {
		prterr("getaddrinfo: %s.", gai_strerror(e));
		exit(1);
	}
	if (ai == NULL) {
		prterr("getaddrinfo: no address found.");
		exit(1);
	}

	/* Opens the socket for listening */
	fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if (fd < 0) {
		perror("socket");
		exit(1);
	}
	if (bind(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
		perror("bind");
		exit(1);
	}
	if (listen(fd, 10) < 0) {
		perror("listen");
		exit(1);
	}

	sin = (struct sockaddr_in *)ai->ai_addr;
	inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
	printf("listening on %s:%d...\n", ip, ntohs(sin->sin_port));

	return fd;
}

static void
usage(void)
{
	extern char *__progname;

	fprintf(stderr, "usage: %s [-p port]\n", __progname);
	exit(1);
}

