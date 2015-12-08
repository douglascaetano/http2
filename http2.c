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
static int http2_frame_send(struct http2_frame *);

static int http2_frame_settings_handler(struct http2_frame *);
static int http2_frame_settings_send(struct http2_connection *, struct http2_setting *, int, int);

/* Frame handlers */
struct http2_frame_handler http2_frame_handlers[] = {
	{ HTTP2_FRAME_SETTINGS, http2_frame_settings_handler },
	{ -1, NULL }
};

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

	/* Receives remaining bytes, if there is any to receive */
	len = fr->fr_header.fh_length - fr->fr_buflen;
	if (len != 0) {
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
	}

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
		bytes = send(sockfd, buf, sizeof(buf), 0);
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

		prtinfo("(%d) Header for frame of type 0x%02x was sent.",
		    sockfd, fr->fr_header.fh_type);
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

		prtinfo("(%d) Frame of type 0x%02x was fully sent. (size=%d)",
		    sockfd, fr->fr_header.fh_type, fr->fr_header.fh_length);

		http2_frame_free(fr);

		/* Returns without rearming writing event if no other frames
		 * are to be sent */
		if (next == NULL) {
			conn->cn_txframe = NULL;
			return;
		}

		conn->cn_txframe = next;
	}

	/* Rearms writing event */
	if (event_add(conn->cn_wrevent, NULL) < 0) {
		prterr("event_add: failure.");
		goto error;
	}
	return;

error:
	http2_connection_free(conn);
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
	struct http2_frame_handler *fh;

	if (fr == NULL)
		return -1;

	prtinfo("(%d) RX frame: len=%d type=%02x flags=%02x stream=%d\n",
	    fr->fr_conn->cn_sockfd, fr->fr_header.fh_length, fr->fr_header.fh_type,
	    fr->fr_header.fh_flags, fr->fr_header.fh_streamid);

	/* Looks for and calls handler for this frame type */
	for (fh = http2_frame_handlers; fh->fh_type != -1; fh++)
		if (fh->fh_type == fr->fr_header.fh_type)
			return fh->fh_handler(fr);

	/* Not supported frames must be ignored and discarded */
	prtinfo("(%d) Unsupported frame type - ignored.", fr->fr_conn->cn_sockfd);
	http2_frame_free(fr);
	return 0;
}

static int
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

	prtinfo("(%d) Frame of type 0x%02x enqueued for sending. (size=%d)",
	    fr->fr_conn->cn_sockfd, fr->fr_header.fh_type, fr->fr_header.fh_length);

	/* Arms writing event */
	if (event_add(fr->fr_conn->cn_wrevent, NULL) < 0) {
		prterr("event_add: failure.");
		http2_connection_free(fr->fr_conn);
		return -1;
	}

	return 0;
}

static int
http2_frame_settings_handler(struct http2_frame *fr)
{
	int pos;

	/* Checks frame size */
	if ((!(fr->fr_header.fh_flags & HTTP2_FRAME_SETTINGS_ACK) &&
	    fr->fr_header.fh_length % HTTP2_FRAME_SETTINGS_PARAM_SIZE != 0) ||
	    (fr->fr_header.fh_flags & HTTP2_FRAME_SETTINGS_ACK &&
	    fr->fr_header.fh_length != 0)) {
		/* TODO connection error: FRAME_SIZE_ERROR */
		prtinfo("(%d) Connection error: "
		    "SETTINGS frame with wrong frame size "
		    "(size=%d,ack=%d)",
		    fr->fr_conn->cn_sockfd, fr->fr_header.fh_length,
		    fr->fr_header.fh_flags & HTTP2_FRAME_SETTINGS_ACK);
		return -1;
	}

	/* Checks stream ID */
	if (fr->fr_header.fh_streamid != 0) {
		/* TODO connection error: PROTOCOL_ERROR */
		prtinfo("(%d) Connection error: "
		    "SETTINGS frame with wrong stream ID "
		    "(id=%d)",
		    fr->fr_conn->cn_sockfd,
		    fr->fr_header.fh_streamid);
		return -1;
	}

	/* On ACK reception, the new requested frames can be set definitely */
	if (fr->fr_header.fh_flags & HTTP2_FRAME_SETTINGS_ACK) {
		/* TODO set new settings definitely */
		prtinfo("(%d) Previously sent SETTINGS frame acknowledged.",
		    fr->fr_conn->cn_sockfd);
		return 0;
	}

	prtinfo("(%d) SETTINGS frame received with %d setting(s).",
	    fr->fr_conn->cn_sockfd,
	    fr->fr_header.fh_length / HTTP2_FRAME_SETTINGS_PARAM_SIZE);

	/* Saves remote's settings */
	for (pos = 0; pos < fr->fr_header.fh_length;
	    pos += HTTP2_FRAME_SETTINGS_PARAM_SIZE) {
		struct http2_setting set;
		char *ptr;

		ptr = &fr->fr_buf[pos];

		set.set_id = ptr[0] << 8 | ptr[1];
		set.set_value = ptr[2] << 24 | ptr[3] << 16 | ptr[4] << 8 | ptr[5];
		/* TODO set settings!
		http2_setting_set(set);
		*/
		prtinfo("(%d) New setting: "
		    "[0x%04x] = 0x%08x.",
		    fr->fr_conn->cn_sockfd,
		    set.set_id, set.set_value);
	}

	/* Sends ACK to remote peer */
	if (http2_frame_settings_send(fr->fr_conn, NULL, 0, 1) < 0) {
		prterr("http2_frame_settings_send: failure.");
		return -1;
	}

	prtinfo("(%d) SETTINGS ACK frame sent back to remote.",
	    fr->fr_conn->cn_sockfd);

	http2_frame_free(fr);

	return 0;
}

static int
http2_frame_settings_send(struct http2_connection *conn,
    struct http2_setting *set, int nsets, int ack)
{
	struct http2_frame *fr;

	fr = http2_frame_new(conn);
	if (fr == NULL) {
		prterr("http2_frame_new: failure.");
		return -1;
	}

	fr->fr_header.fh_type = HTTP2_FRAME_SETTINGS;
	fr->fr_header.fh_streamid = 0;

	if (ack) {
		/* Creates an empty frame with only the ACK flag set */
		fr->fr_header.fh_length = 0;
		fr->fr_header.fh_flags = HTTP2_FRAME_SETTINGS_ACK;
	}
	else
		fr->fr_header.fh_length = nsets * HTTP2_FRAME_SETTINGS_PARAM_SIZE;

	prtinfo("(%d) SETTINGS frame being sent (nsets=%d,ack=%d).",
	    conn->cn_sockfd, nsets, ack);

	if (http2_frame_send(fr) < 0) {
		prterr("http2_frame_send: failure.");
		return -1;
	}

	return 0;
}

int
http2_settings_send(struct http2_connection *conn, struct http2_setting *set, int nsets)
{
	return http2_frame_settings_send(conn, set, nsets, 0);
}

