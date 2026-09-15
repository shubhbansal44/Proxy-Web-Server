#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

int main(int argc, char *argv[])
{
    struct addrinfo HINTS, *ITERATOR, *RESULT;
    int STATUS_CODE, PORT_NUMBER;

    if(argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 1;
    } else {
        PORT_NUMBER = argv[1];
    }
    
    memset(&HINTS, 0, sizeof HINTS);
    HINTS.ai_family = AF_UNSPEC;
    HINTS.ai_socktype = SOCK_STREAM;


} 