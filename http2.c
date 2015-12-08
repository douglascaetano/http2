/**
 * HTTP/2 implementation
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

#include "http2.h"

static void http2_frame_free(struct http2_frame *);

static void http2_connection_read(evutil_socket_t, short, void *);
static void http2_connection_write(evutil_socket_t, short, void *);

static int http2_frame_recv(struct http2_frame *);

struct http2_connection *
http2_connection_new(int sockfd, struct event_base *evbase)
{
	struct http2_connection *conn;

	/* Allocates structure */
	conn = calloc(1, sizeof(*conn));
	if (conn == NULL) {
		prterrno("calloc");
		return NULL;
	}

	conn->cn_sockfd = sockfd;
	conn->cn_event = event_new(evbase, sockfd, EV_READ,
	    http2_connection_read, conn);
	if (conn->cn_event == NULL) {
		prterr("event_new: failure.");
		free(conn);
		return NULL;
	}

	if (event_add(conn->cn_event, NULL) < 0) {
	/* Arms reading event */
		prterr("event_add: failure.");
		event_free(conn->cn_event);
		free(conn);
		return NULL;
	}

	return conn;
}

void
http2_connection_free(struct http2_connection *conn)
{
	struct http2_frame *fr;

	if (conn == NULL)
		return;

	close(conn->cn_sockfd);

	event_free(conn->cn_event);

	http2_frame_free(conn->cn_rxframe);

	fr = conn->cn_txframe;
	while (fr != NULL) {
		struct http2_frame *next;

		next = fr->fr_next;
		http2_frame_free(fr);
		fr = next;
	}

	free(conn);
}

struct http2_frame *
http2_frame_new(struct http2_connection *conn)
{
	struct http2_frame *fr;

	fr = calloc(1, sizeof(*fr));
	if (fr == NULL) {
		perror("calloc");
		return NULL;
	}

	fr->fr_conn = conn;

	return fr;
}

static void
http2_frame_free(struct http2_frame *fr)
{
	if (fr == NULL)
		return;

	if (fr->fr_event != NULL)
		event_free(fr->fr_event);

	free(fr->fr_buf);
	free(fr);
}

static void
http2_connection_read(evutil_socket_t sockfd, short events, void *arg)
{
	struct http2_connection *conn;
	struct http2_frame *fr;
	ssize_t bytes;
	size_t len;

	conn = arg;

	/* New frame received */
	if (conn->cn_rxframe == NULL) {
		char buf[HTTP2_FRAME_HEADER_SIZE];

		/* Receives header */
		bytes = recv(sockfd, buf, sizeof(buf), 0);
		if (bytes < 0) {
			prterrno("recv");
			goto error;
		}
		else if (bytes == 0) {
			prterr("recv: connection was closed.");
			goto error;
		}
		else if (bytes < sizeof(buf)) {
			/* TODO handle this */
			prterr("recv: imcomplete reception of header.");
			goto error;
		}

		/* Creates a new frame */
		conn->cn_rxframe = http2_frame_new(conn);
		if (conn->cn_rxframe == NULL) {
			prterr("http2_frame_new: failure.");
			goto error;
		}

		/* Fills frame header structure */
		conn->cn_rxframe->fr_header.fh_length =
		    buf[0] << 16 | buf[1] << 8 | buf[2];
		conn->cn_rxframe->fr_header.fh_type = buf[3];
		conn->cn_rxframe->fr_header.fh_flags = buf[4];
		conn->cn_rxframe->fr_header.fh_streamid =
		    (buf[5] & 0x7F) << 24 | buf[6] << 16 | buf[7] << 8 | buf[8];

		/* Allocates buffer */
		conn->cn_rxframe->fr_buf = malloc(conn->cn_rxframe->fr_header.fh_length);
		if (conn->cn_rxframe->fr_buf == NULL) {
			perror("malloc");
			goto error;
		}
		conn->cn_rxframe->fr_buflen = 0;
	}

	fr = conn->cn_rxframe;

	/* Receives remaining bytes */
	len = fr->fr_header.fh_length - fr->fr_buflen;
	bytes = recv(sockfd, &fr->fr_buf[fr->fr_buflen], len, 0);
	if (bytes < 0) {
		prterrno("recv");
		goto error;
	}
	else if (bytes == 0) {
		prterr("recv: connection was closed.");
		goto error;
	}

	fr->fr_buflen += bytes;

	/* Checks buffer overflow */
	if (fr->fr_buflen > fr->fr_header.fh_length) {
		prterr("http2_connection_read: frame buffer overflow!");
		goto error;
	}

	/* Handles fully received frame */
	if (fr->fr_buflen == fr->fr_header.fh_length) {
		if (http2_frame_recv(fr) < 0) {
			prterr("http2_frame_recv: failure.");
			goto error;
		}
		conn->cn_rxframe = NULL;
	}

	if (event_add(conn->cn_event, NULL) < 0) {
		prterr("event_add: failure.");
		goto error;
	}
	return;

error:
	http2_connection_free(conn);
	return;
}

static void
http2_connection_write(evutil_socket_t sockfd, short events, void *arg)
{
	struct http2_frame *fr;
	ssize_t bytes;
	size_t len;

	fr = arg;

	len = fr->fr_header.fh_length - fr->fr_buflen;
	bytes = send(sockfd, &fr->fr_buf[fr->fr_buflen], len, MSG_DONTWAIT);
	if (bytes < 0) {
		/* outbound traffic is clogged */
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			goto end;
		else {
			perror("send");
			goto error;
		}
	}

	fr->fr_buflen += bytes;

	/* Checks buffer overflow */
	if (fr->fr_buflen > fr->fr_header.fh_length) {
		prterr("http2_connection_write: frame buffer overflow!");
		goto error;
	}

	/* Frees fully sent frame and sends next on list */
	if (fr->fr_buflen == fr->fr_header.fh_length) {
		struct http2_frame *next;

		next = fr->fr_next;

		http2_frame_free(fr);

		/* Returns without rearming writing event if no other frames
		 * are to be sent */
		if (next == NULL)
			return;

		if (http2_frame_send(next) < 0) {
			prterr("http2_frame_send: failure.");
			goto error;
		}
	}

end:
	if (event_add(fr->fr_event, NULL) < 0) {
		prterr("event_add: failure.");
		goto error;
	}
	return;

error:
	http2_connection_free(fr->fr_conn);
	return;
}

static int
http2_frame_recv(struct http2_frame *fr)
{
	struct http2_frame_header *hdr;

	if (fr == NULL)
		return -1;

	hdr = &fr->fr_header;

	printf("(%d) RX frame: len=%d type=%02x flags=%02x stream=%d\n",
	    fr->fr_conn->cn_sockfd, hdr->fh_length, hdr->fh_type,
	    hdr->fh_flags, hdr->fh_streamid);

	printf("%s\n", fr->fr_buf);

	http2_frame_free(fr);
	return 0;
}

int
http2_frame_send(struct http2_frame *fr)
{
	struct http2_frame_header *fh;
	char buf[HTTP2_FRAME_HEADER_SIZE];
	ssize_t bytes;

	if (fr == NULL)
		return -1;

	fh = &fr->fr_header;

	/* Creates header and sends it */
	/* length */
	buf[0] = (fh->fh_length & 0xFF0000U) >> 16;
	buf[1] = (fh->fh_length & 0x00FF00U) >>  8;
	buf[2] = (fh->fh_length & 0x0000FFU);
	/* type */
	buf[3] = fh->fh_type;
	/* flags */
	buf[4] = fh->fh_flags;
	/* reserved bit + stream id */
	buf[5] = (fh->fh_streamid & 0x7F000000U) >> 24;
	buf[6] = (fh->fh_streamid & 0x00FF0000U) >> 16;
	buf[7] = (fh->fh_streamid & 0x0000FF00U) >>  8;
	buf[8] = (fh->fh_streamid & 0x000000FFU);
	bytes = send(fr->fr_conn->cn_sockfd, buf, sizeof(buf), 0);
	if (bytes < 0) {
		perror("send");
		goto error;
	}
	else if (bytes < sizeof(buf)) {
		/* TODO handle this */
		prterr("send: incomplete transmission of header.");
		goto error;
	}

	if (fr->fr_event != NULL)
		event_free(fr->fr_event);

	fr->fr_event = event_new(
	    event_get_base(fr->fr_conn->cn_event),
	    fr->fr_conn->cn_sockfd,
	    EV_WRITE,
	    http2_connection_write,
	    fr);

	if (fr->fr_event == NULL) {
		prterr("event_new: failure.");
		goto error;
	}

	if (event_add(fr->fr_event, NULL) < 0) {
		prterr("event_add: failure.");
		goto error;
	}

	return 0;

error:
	http2_connection_free(fr->fr_conn);
	return -1;
}

