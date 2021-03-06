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
#include "http2.h"
#include "util.h"

#include "client.h"

/* libevent's structures */
struct event_base *evbase;

static void usage(void);

int
main(int argc, char *argv[])
{
	struct http2_connection *conn;
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
	if (optind >= argc) {
		prterr("missing host to connect to.");
		usage();
	}
	host = argv[optind];

	printf("HTTP/2 client\n");

	/* Opens the socket */
	sockfd = client_connect(host, port);
	if (sockfd < 0) {
		prterr("client_connect: failure.");
		exit(1);
	}

	/* Creates libevent event_base struct */
	evbase = event_base_new();
	if (evbase == NULL) {
		close(sockfd);
		prterr("event_base_new: failure.");
		exit(1);
	}

	/* Creates a new HTTP/2 connection */
	conn = http2_connection_new(sockfd, evbase);
	if (conn == NULL) {
		close(sockfd);
		prterr("http2_connection_new: failure.");
		exit(1);
	}

	/* Sends client preface: sends starting sequence and a SETTINGS frame */
	/* TODO send starting sequence */
	http2_settings_send(conn, NULL, 0);

	/* Dispatch events */
	r = event_base_dispatch(evbase);

	if (r < 0) {
		prterr("event_base_dispatch: failure.");
		http2_connection_free(conn);
	}

	event_base_free(evbase);

	return 0;
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
		prterr("getaddrinfo: %s.", gai_strerror(e));
		exit(1);
	}
	if (ai == NULL) {
		prterr("getaddrinfo: no address found.");
		exit(1);
	}

	/* Opens the socket and connect to the host */
	fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if (fd < 0) {
		prterrno("socket");
		exit(1);
	}
	if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
		prterrno("connect");
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

