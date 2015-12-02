/**
 * HTTP/2 minimalistic client
 */

#ifndef __CLIENT_H__
#define __CLIENT_H__

void client_read(evutil_socket_t, short, void *);
int client_connect(char *, char *);

#endif /* !__CLIENT_H__ */

