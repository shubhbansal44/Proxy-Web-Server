#include "proxy_parse.h"
#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <bits/time.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 10
#define MAX_BYTES 4096
typedef struct CacheModule CacheModule;

struct CacheModule {
  char *DATA;
  int LENGTH;
  char *URL;
  time_t UPTIME;
  CacheModule *NEXT;
};

CacheModule *FindCache(char *URL);

int AddCache(char *DATA, int SIZE, char *URL);

void RemoveCache();

int PORT_NUMBER = 8080;
int PROXY_SOCKET_ID;
pthread_t THREAD_ID[MAX_CLIENTS];
sem_t SEMAPHORE;
pthread_mutex_t LOCK;

CacheModule *HEAD;
int CACHE_SIZE;

int HandleRequest(int CLIENT_SOCKET_ID, struct ParsedRequest* CLIENT_PARSED_REQUEST, char* CLIENT_REQUEST) {
  char* BUFFER = (char*)malloc(MAX_BYTES * sizeof(char));
  strcpy(BUFFER, "GET ");
  strcat(BUFFER, CLIENT_PARSED_REQUEST->path);
  strcat(BUFFER, " ");
  strcat(BUFFER, CLIENT_PARSED_REQUEST->version);
  strcat(BUFFER, "\r\n");
  size_t LENGTH = strlen(BUFFER);

  if(ParsedHeader_set(CLIENT_PARSED_REQUEST, "Connection", "close") < 0) {
    printf("Error Occured While Establising Parsed request Connection!");
  }

  if(ParsedHeader_get(CLIENT_PARSED_REQUEST, "Host") == NULL) {
    if(ParsedHeader_set(CLIENT_PARSED_REQUEST, "Host", CLIENT_PARSED_REQUEST->host) < 0) {
      printf("Error Occured While Setting Host in Parsed Request Header!");
    }
  }

  return 0;
}

void *THREAD_ROUTINE(void *NEW_SOCKET) {
  sem_wait(&SEMAPHORE);
  int CURRENT_SEMAPHORE_VALUE;
  sem_getvalue(&SEMAPHORE, &CURRENT_SEMAPHORE_VALUE);
  printf("Currently Available Sockets: %d\n", CURRENT_SEMAPHORE_VALUE);

  int *NEW_SOCKET_PTR = (int *)NEW_SOCKET;
  int SOCKET = *NEW_SOCKET_PTR;
  int BYTES_RECIEVED, LENGTH;

  char *BUFFER = (char *)calloc(MAX_BYTES, sizeof(char));
  bzero(BUFFER, MAX_BYTES);

  BYTES_RECIEVED = recv(SOCKET, BUFFER, MAX_BYTES, 0);
  while (BYTES_RECIEVED > 0) {
    LENGTH = strlen(BUFFER);
    if (strstr(BUFFER, "\r\n\r\n") == NULL) {
      BYTES_RECIEVED = recv(SOCKET, BUFFER + LENGTH, MAX_BYTES - LENGTH, 0);
    } else {
      break;
    }
  }

  char *REQUEST = (char *)malloc(strlen(BUFFER) * sizeof(char) + 1);
  for (int i = 0; i < strlen(BUFFER); i++) {
    REQUEST[i] = BUFFER[i];
  }

  CacheModule *CACHE = FindCache(REQUEST);
  if (CACHE != NULL) {
    int SIZE = CACHE->LENGTH / sizeof(char);
    int POS = 0;
    char RESPONSE[MAX_BYTES];
    while(POS < SIZE) {
      bzero(RESPONSE, MAX_BYTES);
      for (int i = 0; i < MAX_BYTES; i++) {
        RESPONSE[i] = CACHE->DATA[i];
        POS++;
      }
      send(SOCKET, RESPONSE, MAX_BYTES, 0);
    }
    printf("Data Retrived From Cache\n");
    printf("%s\n\n", RESPONSE);
  } else if (BYTES_RECIEVED > 0) {
    LENGTH = strlen(BUFFER);
    struct ParsedRequest* PARSED_REQUEST = ParsedRequest_create();

    if(ParsedRequest_parse(PARSED_REQUEST, BUFFER, LENGTH) < 0) {
      printf("Parsing Failed!");
    } else {
      bzero(BUFFER, MAX_BYTES);
      if(!strcmp(PARSED_REQUEST->method, "GET")) {
        if(PARSED_REQUEST->host && PARSED_REQUEST->path && checkHTTPversion(PARSED_REQUEST->version) == 1) {
          BYTES_RECIEVED = HandleRequest(SOCKET, PARSED_REQUEST, REQUEST);
          if(BYTES_RECIEVED == -1) {
            ThrowError(SOCKET, 500);
          }
        } else {
          ThrowError(SOCKET, 500);
        }
      } else {
        printf("Can't Handle Request Other Than \'GET\'");
      }
    }
    ParsedRequest_destroy(PARSED_REQUEST);
  } else if(BYTES_RECIEVED == 0) {
    printf("Request Didn't Received, User May Be Disconnected");
  }
  shutdown(SOCKET, SHUT_RDWR);
  close(SOCKET);
  free(BUFFER);

  sem_post(&SEMAPHORE);
  sem_getvalue(&SEMAPHORE, &CURRENT_SEMAPHORE_VALUE);
  printf("Currently Available Sockets: %d\n", CURRENT_SEMAPHORE_VALUE);

  free(REQUEST);
  return NULL;
}

int main(int argc, char *argv[]) {

  int CLIENT_SOCKET_ID, CLIENT_LENGTH;
  struct sockaddr_in SERVER_ADDR, CLIENT_ADDR;
  sem_init(&SEMAPHORE, 0, MAX_CLIENTS);
  pthread_mutex_init(&LOCK, NULL);

  if (argc == 2) {
    PORT_NUMBER = atoi(argv[1]);
  } else {
    fprintf(stderr, "Usage: %s <port>\n", argv[0]);
    exit(1);
  }

  printf("Starting Proxy Server At Port: %d...\n", PORT_NUMBER);

  PROXY_SOCKET_ID = socket(AF_INET, SOCK_STREAM, 0);
  if (PROXY_SOCKET_ID < 0) {
    perror("Failed To Create Proxy Socket ID!");
    exit(1);
  }

  int REUSE = 1;
  if (setsockopt(PROXY_SOCKET_ID, SOL_SOCKET, SO_REUSEADDR,
                 (const char *)&REUSE, sizeof(REUSE)) < 0) {
    perror("Execution Failed While Setting Socket Option(setsockopt)!");
  }

  bzero((char *)&SERVER_ADDR, sizeof(SERVER_ADDR));
  SERVER_ADDR.sin_family = AF_INET;
  SERVER_ADDR.sin_port = htons(PORT_NUMBER);
  SERVER_ADDR.sin_addr.s_addr = INADDR_ANY;

  if (bind(PROXY_SOCKET_ID, (struct sockaddr *)&SERVER_ADDR,
           sizeof(SERVER_ADDR)) < 0) {
    perror("Port Is Not Available!");
    exit(0);
  }
  printf("Binding On Port: %d/n", PORT_NUMBER);
  int LISTEN_STATUS = listen(PROXY_SOCKET_ID, MAX_CLIENTS);

  if (LISTEN_STATUS < 0) {
    perror("Error Occured While Listening!");
    exit(1);
  }

  int ITERATOR = 0;
  int CONNECTED_SOCKET_ID[MAX_CLIENTS];

  while (1) {
    bzero((char *)&CLIENT_ADDR, sizeof(CLIENT_ADDR));
    CLIENT_LENGTH = sizeof(CLIENT_ADDR);
    CLIENT_SOCKET_ID = accept(PROXY_SOCKET_ID, (struct sockaddr *)&CLIENT_ADDR,
                              (socklen_t *)&CLIENT_LENGTH);

    if (CLIENT_SOCKET_ID < 0) {
      perror("Unable To Connect New User!");
      exit(1);
    } else {
      CONNECTED_SOCKET_ID[ITERATOR] = CLIENT_SOCKET_ID;
    }

    struct sockaddr_in *CLIENT_PTR = (struct sockaddr_in *)&CLIENT_ADDR;
    struct in_addr IP_ADDR = CLIENT_PTR->sin_addr;
    char str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &IP_ADDR, str, INET_ADDRSTRLEN);
    printf("Client Is Connected Via Port Number: %d And IP Address: %s\n",
           ntohs(CLIENT_ADDR.sin_port), str);

    pthread_create(&THREAD_ID[ITERATOR], NULL, THREAD_ROUTINE,
                   (void *)&CONNECTED_SOCKET_ID[ITERATOR]);
    ITERATOR += 1;
  }
  close(PROXY_SOCKET_ID);
  return 0;
}