#ifndef TLS_TUNNEL_H
#define TLS_TUNNEL_H

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>

void InitOpenSSL();
void CleanupOpenSSL();
int HandleConnect(int client_socket, const char *host, int port);

#endif
