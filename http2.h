/**
 * HTTP/2 implementation
 */

#ifndef __HTTP2_H__
#define __HTTP2_H__

#define HTTP2_FRAME_HEADER_SIZE 9

/* Frames types */
#define HTTP2_FRAME_SETTINGS 0x04

/* SETTINGS frame flags */
#define HTTP2_FRAME_SETTINGS_ACK 0x01

#define HTTP2_FRAME_SETTINGS_PARAM_SIZE 6

struct http2_frame;

typedef int (*http2_frame_handler_f)(struct http2_frame *);

struct http2_frame_handler {
	int fh_type;
	http2_frame_handler_f fh_handler;
};

/**
 * Frame structure
 *
 * fr_buflen:
 *   Informs how much of data on buffer was already filled/consumed.
 *   If fr_buflen == -1, header has not yet been received/sent. In this case, when
 *   receiving, buffer will not be allocated yet.
 */
struct http2_frame {
	struct http2_connection *fr_conn;
	size_t fr_length;
	uint8_t fr_type;
	uint8_t fr_flags;
	uint32_t fr_streamid;
	char *fr_buf;
	size_t fr_buflen;
	struct http2_frame *fr_next;
};

struct http2_setting {
	uint16_t set_id;
	uint32_t set_value;
};

struct http2_connection {
	int cn_sockfd;
	struct event *cn_rdevent;
	struct event *cn_wrevent;
	struct http2_setting *cn_remsets; /* settings from remote peer */
	struct http2_setting *cn_locsets; /* local settings */
	struct http2_setting *cn_locsets_nack; /* local settings not ACK'ed */
	struct http2_frame *cn_rxframe; /* currently being recepted frame */
	struct http2_frame *cn_txframe; /* currently being sent frame */
	struct http2_frame *cn_txlastframe; /* last frame to be sent on list */
};

struct http2_connection *http2_connection_new(int, struct event_base *);
void http2_connection_free(struct http2_connection *);

struct http2_frame *http2_frame_new(struct http2_connection *);

int http2_settings_send(struct http2_connection *, struct http2_setting *, int);

#endif /* !__HTTP2_H__ */

