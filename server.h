/**
 * HTTP/2 minimalistic server
 */

#ifndef __SERVER_H__
#define __SERVER_H__

void server_accept(evutil_socket_t, short, void *);
int server_listen(char *);

#endif /* !__SERVER_H__ */

