/**
 * HTTP/2 implementation
 */

#ifndef __HTTP2_H__
#define __HTTP2_H__

#define HTTP2_FRAME_HEADER_SIZE 9

struct http2_frame_header {
	size_t fh_length;
	char fh_type;
	char fh_flags;
	uint32_t fh_streamid;
};

struct http2_frame {
	struct http2_connection *fr_conn;
	struct event *fr_event;
	struct http2_frame_header fr_header;
	char *fr_buf;
	size_t fr_buflen; /* data length on buffer */
	struct http2_frame *fr_next;
};

struct http2_connection {
	int cn_sockfd;
	struct event *cn_event;
	struct http2_frame *cn_rxframe;
	struct http2_frame *cn_txframe;
	struct http2_frame *cn_txlastframe;
};

struct http2_connection *http2_connection_new(int, struct event_base *);
void http2_connection_free(struct http2_connection *);

struct http2_frame *http2_frame_new(struct http2_connection *);
int http2_frame_send(struct http2_frame *);

#endif /* !__HTTP2_H__ */

