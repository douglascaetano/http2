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

static void http2_connection_read(evutil_socket_t, short, void *);
static void http2_connection_write(evutil_socket_t, short, void *);

static void http2_frame_free(struct http2_frame *);

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

	/* Sets initial values */
	conn->cn_sockfd = sockfd;

	/* Creates events for socket's reading and writing readiness */
	conn->cn_rdevent = event_new(evbase, sockfd, EV_READ,
	    http2_connection_read, conn);
	conn->cn_wrevent = event_new(evbase, sockfd, EV_WRITE,
	    http2_connection_write, conn);
	if (conn->cn_rdevent == NULL || conn->cn_wrevent == NULL) {
		prterr("event_new: failure.");
		free(conn);
		return NULL;
	}

	/* Arms reading event */
	if (event_add(conn->cn_rdevent, NULL) < 0) {
		prterr("event_add: failure.");
		event_free(conn->cn_rdevent);
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

	event_free(conn->cn_rdevent);
	event_free(conn->cn_wrevent);

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

	/* Rearms reading event */
	if (event_add(conn->cn_rdevent, NULL) < 0) {
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
	struct http2_connection *conn;
	struct http2_frame *fr;
	ssize_t bytes;
	size_t len;

	conn = arg;
	fr = conn->cn_txframe;

	/* If not sent, creates header and sends it */
	if (fr->fr_buflen == -1) {
		char buf[HTTP2_FRAME_HEADER_SIZE];

		/* length */
		buf[0] = (fr->fr_header.fh_length & 0xFF0000U) >> 16;
		buf[1] = (fr->fr_header.fh_length & 0x00FF00U) >>  8;
		buf[2] = (fr->fr_header.fh_length & 0x0000FFU);
		/* type */
		buf[3] = fr->fr_header.fh_type;
		/* flags */
		buf[4] = fr->fr_header.fh_flags;
		/* reserved bit + stream id */
		buf[5] = (fr->fr_header.fh_streamid & 0x7F000000U) >> 24;
		buf[6] = (fr->fr_header.fh_streamid & 0x00FF0000U) >> 16;
		buf[7] = (fr->fr_header.fh_streamid & 0x0000FF00U) >>  8;
		buf[8] = (fr->fr_header.fh_streamid & 0x000000FFU);
		bytes = send(fr->fr_conn->cn_sockfd, buf, sizeof(buf), 0);
		if (bytes < 0) {
			prterrno("send");
			goto error;
		}
		else if (bytes < sizeof(buf)) {
			/* TODO handle this */
			prterr("send: incomplete transmission of header.");
			goto error;
		}
		fr->fr_buflen = 0;
	}

	/* Sends remaining bytes */
	len = fr->fr_header.fh_length - fr->fr_buflen;
	bytes = send(sockfd, &fr->fr_buf[fr->fr_buflen], len, 0);
	if (bytes < 0) {
		prterrno("send");
		goto error;
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

	/* Rearms writing event */
	if (event_add(fr->fr_conn->cn_wrevent, NULL) < 0) {
		prterr("event_add: failure.");
		goto error;
	}
	return;

error:
	http2_connection_free(fr->fr_conn);
	return;
}

struct http2_frame *
http2_frame_new(struct http2_connection *conn)
{
	struct http2_frame *fr;

	fr = calloc(1, sizeof(*fr));
	if (fr == NULL) {
		prterrno("calloc");
		return NULL;
	}

	fr->fr_conn = conn;
	fr->fr_buflen = -1;

	return fr;
}

/**
 * http2_frame_free() does not and should not free next frames on list.
 */
static void
http2_frame_free(struct http2_frame *fr)
{
	if (fr == NULL)
		return;

	free(fr->fr_buf);
	free(fr);
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
	if (fr == NULL)
		return -1;

	/* Enqueues frame */
	if (fr->fr_conn->cn_txframe == NULL)
		fr->fr_conn->cn_txframe = fr;
	else
		fr->fr_conn->cn_txlastframe->fr_next = fr;
	fr->fr_conn->cn_txlastframe = fr;

	/* Arms writing event */
	if (event_add(fr->fr_conn->cn_wrevent, NULL) < 0) {
		prterr("event_add: failure.");
		http2_connection_free(fr->fr_conn);
		return -1;
	}

	return 0;
}

