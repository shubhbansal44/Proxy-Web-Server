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

// Number of clients/requests a proxy server can handle.
#define MAX_CLIENTS 10

// Max size of single request.
#define MAX_BYTES 4096

#define MAX_ELEMENT_SIZE 10 * (1 << 10)

#define MAX_CACHE_SIZE 200 * (1 << 20)

// Cache Module declaration
// DATA Buffer stores received data,
// LENGTH tells about size of DATA,
// URL - address asssociated with the request,
// UPTIME signifies request's recency,
// NEXT Pointer stores next cache module address.
typedef struct CacheModule
{
  char *DATA;
  int LENGTH;
  char *URL;
  time_t UPTIME;
  CacheModule *NEXT;
} CacheModule;

CacheModule *FindCache(char *URL);

int AddCache(char *DATA, int SIZE, char *URL);

void RemoveCache();

// Server's port number
int PORT_NUMBER = 8080;

// Server's socket id
int PROXY_SOCKET_ID;

// Buffer to store Thread IDs associated with each client's request.
pthread_t THREAD_ID[MAX_CLIENTS];

// semaphore lock for handling multiple (MAX CLIENTS) users.
sem_t SEMAPHORE;

// mutex lock for handling cache read/write.
pthread_mutex_t LOCK;

// global cache head
CacheModule *HEAD;

int CACHE_SIZE;

int ConnectEndServer(void *hostname, int port)
{
  int END_SERVER_SOCKET = socket(AF_INET, SOCK_STREAM, 0);
  if (END_SERVER_SOCKET < 0)
  {
    fprintf(stderr, "ConnectEndServer: Something went wrong while inializing end server socket!\n");
    return -1;
  }

  struct hostent *HOST = gethostbyname((const char *)hostname);
  if (HOST == NULL)
  {
    fprintf(stderr, "ConnectEndServer: No such host exists: %s\n", (char *)hostname);
    close(END_SERVER_SOCKET);
    return -1;
  }

  struct sockaddr_in END_SERVER_ADDR;
  memset(&END_SERVER_ADDR, 0, sizeof(END_SERVER_ADDR));
  END_SERVER_ADDR.sin_family = AF_INET;
  END_SERVER_ADDR.sin_port = htons(port);

  /* copy first resolved address into sin_addr */
  memcpy(&END_SERVER_ADDR.sin_addr, HOST->h_addr_list[0], HOST->h_length);

  if (connect(END_SERVER_SOCKET, (struct sockaddr *)&END_SERVER_ADDR, sizeof(END_SERVER_ADDR)) < 0)
  {
    fprintf(stderr, "ConnectEndServer: connect to end server failed");
    close(END_SERVER_SOCKET);
    return -1;
  }

  return END_SERVER_SOCKET;
}

int HandleRequest(int CLIENT_SOCKET_ID, struct ParsedRequest *CLIENT_PARSED_REQUEST, char *CLIENT_REQUEST)
{
  char *BUFFER = (char *)malloc(MAX_BYTES * sizeof(char));
  strcpy(BUFFER, "GET ");
  strcat(BUFFER, CLIENT_PARSED_REQUEST->path);
  strcat(BUFFER, " ");
  strcat(BUFFER, CLIENT_PARSED_REQUEST->version);
  strcat(BUFFER, "\r\n");
  size_t LENGTH = strlen(BUFFER);

  if (ParsedHeader_set(CLIENT_PARSED_REQUEST, "Connection", "close") < 0)
  {
    fprintf(stderr, "HandleRequest: Error occured While Establising Parsed request connection!\n");
  }

  if (ParsedHeader_get(CLIENT_PARSED_REQUEST, "Host") == NULL)
  {
    if (ParsedHeader_set(CLIENT_PARSED_REQUEST, "Host", CLIENT_PARSED_REQUEST->host) < 0)
    {
      fprintf(stderr, "HandleRequest: Error occured while setting host in Parsed Request header!\n");
    }
  }

  if (ParsedRequest_unparse_headers(CLIENT_PARSED_REQUEST, BUFFER + LENGTH, (size_t)MAX_BYTES - LENGTH) < 0)
  {
    fprintf(stderr, "HandleRequest: Error occured while unparsing headers!\n");
  }

  int END_SERVER_PORT = 80;
  if (CLIENT_PARSED_REQUEST->port != NULL)
  {
    END_SERVER_PORT = atoi(CLIENT_PARSED_REQUEST->port);
  }

  int END_SERVER_SOCKET_ID = ConnectEndServer(CLIENT_PARSED_REQUEST->host, END_SERVER_PORT);
  if (END_SERVER_SOCKET_ID < 0)
  {
    fprintf(stderr, "HandleRequest: Error occured while creating end server socket ID!\n");
    return -1;
  }

  /* send request to end server */
  ssize_t BYTES_SEND = send(END_SERVER_SOCKET_ID, BUFFER, strlen(BUFFER), 0);
  if (BYTES_SEND < 0)
  {
    fprintf(stderr, "HandleRequest: sending request to end server failed");
    close(END_SERVER_SOCKET_ID);
    return -1;
  }

  /* Receive from end server and stream to client; also build RESPONSE for caching */
  char *RESPONSE = (char *)malloc(MAX_BYTES);
  if (!RESPONSE)
  {
    close(END_SERVER_SOCKET_ID);
    return -1;
  }
  size_t RESPONSE_CAPACITY = MAX_BYTES;
  size_t RESPONSE_LENGTH = 0;

  ssize_t BYTES_RECEIVED;
  while ((BYTES_RECEIVED = recv(END_SERVER_SOCKET_ID, BUFFER, MAX_BYTES, 0)) > 0)
  {
    ssize_t BYTES_SEND_CLIENT = send(CLIENT_SOCKET_ID, BUFFER, BYTES_RECEIVED, 0);
    if (BYTES_SEND_CLIENT < 0)
    {
      fprintf(stderr, "HandleRequest: send to client failed");
      break;
    }
    /* append to RESPONSE buffer */
    if (RESPONSE_LENGTH + (size_t)BYTES_RECEIVED + 1 > RESPONSE_CAPACITY)
    {
      RESPONSE_CAPACITY *= 2;
      char *TEMP = (char *)realloc(RESPONSE, RESPONSE_CAPACITY);
      if (!TEMP)
      {
        fprintf(stderr, "HandleRequest: realloc failed");
        break;
      }
      RESPONSE = TEMP;
    }
    memcpy(RESPONSE + RESPONSE_LENGTH, BUFFER, BYTES_RECEIVED);
    RESPONSE_LENGTH += BYTES_RECEIVED;
  }

  if (BYTES_RECEIVED < 0)
    fprintf(stderr, "HandleRequest: recv from end server failed");

  /* null-terminate for safety */
  if (RESPONSE_LENGTH + 1 > RESPONSE_CAPACITY)
  {
    char *TEMP = (char *)realloc(RESPONSE, RESPONSE_LENGTH + 1);
    if (TEMP)
      RESPONSE = TEMP;
  }
  RESPONSE[RESPONSE_LENGTH] = '\0';

  /* Attempt caching (use the original request string as key) */
  AddCache(RESPONSE, RESPONSE_LENGTH, CLIENT_REQUEST);

  free(BUFFER);
  free(RESPONSE);
  close(END_SERVER_SOCKET_ID);

  return 0;
}

int checkHTTPversion(char *clientVersion)
{
  int version = -1;

  if (strncmp(clientVersion, "HTTP/1.1", 8) == 0)
  {
    version = 1;
  }
  else if (strncmp(clientVersion, "HTTP/1.0", 8) == 0)
  {
    version = 1; // Handling this similar to version 1.1
  }
  else
    version = -1;

  return version;
}

int ThrowError(int socket, int status_code)
{
  char str[1024];
  char currentTime[50];
  time_t now = time(0);

  struct tm data = *gmtime(&now);
  strftime(currentTime, sizeof(currentTime), "%a, %d %b %Y %H:%M:%S %Z", &data);

  switch (status_code)
  {
  case 400:
    snprintf(str, sizeof(str), "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Rqeuest</H1>\n</BODY></HTML>", currentTime);
    printf("400 Bad Request\n");
    send(socket, str, strlen(str), 0);
    break;

  case 403:
    snprintf(str, sizeof(str), "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>", currentTime);
    printf("403 Forbidden\n");
    send(socket, str, strlen(str), 0);
    break;

  case 404:
    snprintf(str, sizeof(str), "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>", currentTime);
    printf("404 Not Found\n");
    send(socket, str, strlen(str), 0);
    break;

  case 500:
    snprintf(str, sizeof(str), "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>", currentTime);
    printf("500 Internal Server Error\n");
    send(socket, str, strlen(str), 0);
    break;

  case 501:
    snprintf(str, sizeof(str), "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>", currentTime);
    printf("501 Not Implemented\n");
    send(socket, str, strlen(str), 0);
    break;

  case 505:
    snprintf(str, sizeof(str), "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>", currentTime);
    printf("505 HTTP Version Not Supported\n");
    send(socket, str, strlen(str), 0);
    break;

  default:
    return -1;
  }
  return 1;
}

void *THREAD_ROUTINE(void *NEW_SOCKET)
{
  sem_wait(&SEMAPHORE);
  int CURRENT_SEMAPHORE_VALUE;
  sem_getvalue(&SEMAPHORE, &CURRENT_SEMAPHORE_VALUE);
  printf("Currently available Sockets: %d\n", CURRENT_SEMAPHORE_VALUE);

  int *NEW_SOCKET_PTR = (int *)NEW_SOCKET;
  int SOCKET = *NEW_SOCKET_PTR;
  int BYTES_RECIEVED, LENGTH;

  char *BUFFER = (char *)calloc(MAX_BYTES, sizeof(char));
  bzero(BUFFER, MAX_BYTES);

  BYTES_RECIEVED = recv(SOCKET, BUFFER, MAX_BYTES, 0);
  while (BYTES_RECIEVED > 0)
  {
    LENGTH = strlen(BUFFER);
    if (strstr(BUFFER, "\r\n\r\n") == NULL)
    {
      BYTES_RECIEVED = recv(SOCKET, BUFFER + LENGTH, MAX_BYTES - LENGTH, 0);
    }
    else
    {
      break;
    }
  }

  char *REQUEST = (char *)malloc(strlen(BUFFER) * sizeof(char) + 1);
  for (size_t i = 0; i < strlen(BUFFER); i++)
  {
    REQUEST[i] = BUFFER[i];
  }

  CacheModule *CACHE = FindCache(REQUEST);
  if (CACHE != NULL)
  {
    int SIZE = CACHE->LENGTH / sizeof(char);
    int POS = 0;
    char RESPONSE[MAX_BYTES];
    while (POS < SIZE)
    {
      bzero(RESPONSE, MAX_BYTES);
      for (int i = 0; i < MAX_BYTES; i++)
      {
        RESPONSE[i] = CACHE->DATA[POS];
        POS++;
      }
      send(SOCKET, RESPONSE, MAX_BYTES, 0);
    }
    printf("Data retrived from cache\n");
    printf("%s\n\n", RESPONSE);
  }
  else if (BYTES_RECIEVED > 0)
  {
    LENGTH = strlen(BUFFER);
    struct ParsedRequest *PARSED_REQUEST = ParsedRequest_create();

    if (ParsedRequest_parse(PARSED_REQUEST, BUFFER, LENGTH) < 0)
    {
      printf("Parsing failed!\n");
    }
    else
    {
      bzero(BUFFER, MAX_BYTES);
      if (!strcmp(PARSED_REQUEST->method, "GET"))
      {
        if (PARSED_REQUEST->host && PARSED_REQUEST->path && checkHTTPversion(PARSED_REQUEST->version) == 1)
        {
          BYTES_RECIEVED = HandleRequest(SOCKET, PARSED_REQUEST, REQUEST);
          if (BYTES_RECIEVED == -1)
          {
            ThrowError(SOCKET, 500);
          }
        }
        else
        {
          ThrowError(SOCKET, 500);
        }
      }
      else
      {
        printf("Can't handle request other than \'GET\'\n");
      }
    }
    ParsedRequest_destroy(PARSED_REQUEST);
  }
  else if (BYTES_RECIEVED == 0)
  {
    printf("Request didn't received, user may be disconnected\n");
  }
  shutdown(SOCKET, SHUT_RDWR);
  close(SOCKET);
  free(BUFFER);

  sem_post(&SEMAPHORE);
  sem_getvalue(&SEMAPHORE, &CURRENT_SEMAPHORE_VALUE);
  printf("Currently available Sockets: %d\n", CURRENT_SEMAPHORE_VALUE);

  free(REQUEST);
  return NULL;
}

CacheModule *FindCache(char *URL)
{
  CacheModule *RESPONSE = NULL;
  int CURRENT_LOCK_VALUE = pthread_mutex_lock(&LOCK);
  printf("Lock acquired %d\n", CURRENT_LOCK_VALUE);
  if (HEAD != NULL)
  {
    RESPONSE = HEAD;
    while (RESPONSE != NULL)
    {
      if (!strcmp(RESPONSE->URL, URL))
      {
        printf("Response uptime: %ld\n", RESPONSE->UPTIME);
        printf("Cache found!\n");
        RESPONSE->UPTIME = time(NULL);
        printf("Response current uptime: %ld\n", RESPONSE->UPTIME);
        break;
      }
      RESPONSE = RESPONSE->NEXT;
    }
  }
  else
  {
    printf("Cache not found\n");
  }
  CURRENT_LOCK_VALUE = pthread_mutex_unlock(&LOCK);
  printf("Lock released %d\n", CURRENT_LOCK_VALUE);
  return RESPONSE;
}

int AddCache(char *DATA, int SIZE, char *URL)
{
  int CURRENT_LOCK_VALUE = pthread_mutex_lock(&LOCK);
  printf("Lock acquired %d\n", CURRENT_LOCK_VALUE);
  int ELEMENT_SIZE = SIZE + strlen(URL) + sizeof(CacheModule) + 1;
  if (ELEMENT_SIZE > MAX_ELEMENT_SIZE)
  {
    CURRENT_LOCK_VALUE = pthread_mutex_unlock(&LOCK);
    printf("Lock released %d\n", CURRENT_LOCK_VALUE);
    return 0;
  }
  else
  {
    while (CACHE_SIZE + ELEMENT_SIZE > MAX_CACHE_SIZE)
    {
      RemoveCache();
    }
    CacheModule *CACHE = (CacheModule *)malloc(sizeof(CacheModule));
    CACHE->DATA = (char *)malloc(SIZE + 1);
    if (!CACHE->DATA)
    {
      fprintf(stderr, "Something went wrong while allocating cache data!\n");
    }
    memcpy(CACHE->DATA, DATA, SIZE);
    CACHE->DATA[SIZE] = '\0';
    CACHE->URL = (char *)malloc(strlen(URL) + 1);
    strcpy(CACHE->URL, URL);
    CACHE->UPTIME = time(NULL);
    CACHE->NEXT = HEAD;
    CACHE->LENGTH = SIZE;
    HEAD = CACHE;
    CACHE_SIZE += ELEMENT_SIZE;
    CURRENT_LOCK_VALUE = pthread_mutex_unlock(&LOCK);
    printf("Lock released %d\n", CURRENT_LOCK_VALUE);
    return 1;
  }
  return 0;
}

void RemoveCache()
{
  pthread_mutex_lock(&LOCK);
  if (!HEAD)
  {
    pthread_mutex_unlock(&LOCK);
    return;
  }

  CacheModule *prev = NULL;
  CacheModule *cur = HEAD;
  CacheModule *lru_prev = NULL;
  CacheModule *lru = HEAD;

  while (cur)
  {
    if (cur->UPTIME < lru->UPTIME)
    {
      lru = cur;
      lru_prev = prev;
    }
    prev = cur;
    cur = cur->NEXT;
  }

  if (lru == HEAD)
  {
    HEAD = HEAD->NEXT;
  }
  else
  {
    lru_prev->NEXT = lru->NEXT;
  }

  CACHE_SIZE -= (sizeof(CacheModule) + strlen(lru->URL) + lru->LENGTH + 1);
  free(lru->DATA);
  free(lru->URL);
  free(lru);
  pthread_mutex_unlock(&LOCK);
}

int main(int argc, char *argv[])
{

  int CLIENT_SOCKET_ID, CLIENT_LENGTH;
  struct sockaddr_in SERVER_ADDR, CLIENT_ADDR;
  sem_init(&SEMAPHORE, 0, MAX_CLIENTS);
  pthread_mutex_init(&LOCK, NULL);

  if (argc == 2)
  {
    PORT_NUMBER = atoi(argv[1]);
  }
  else
  {
    fprintf(stderr, "Usage: %s <port>\n", argv[0]);
    exit(1);
  }

  printf("Starting Proxy Server at Port: %d...\n", PORT_NUMBER);

  PROXY_SOCKET_ID = socket(AF_INET, SOCK_STREAM, 0);
  if (PROXY_SOCKET_ID < 0)
  {
    printf("Failed to create Proxy Socket ID!\n");
    exit(1);
  }

  int REUSE = 1;
  if (setsockopt(PROXY_SOCKET_ID, SOL_SOCKET, SO_REUSEADDR,
                 (const char *)&REUSE, sizeof(REUSE)) < 0)
  {
    printf("Execution failed while setting Socket option(setsockopt)!\n");
  }

  bzero((char *)&SERVER_ADDR, sizeof(SERVER_ADDR));
  SERVER_ADDR.sin_family = AF_INET;
  SERVER_ADDR.sin_port = htons(PORT_NUMBER);
  SERVER_ADDR.sin_addr.s_addr = INADDR_ANY;

  if (bind(PROXY_SOCKET_ID, (struct sockaddr *)&SERVER_ADDR,
           sizeof(SERVER_ADDR)) < 0)
  {
    printf("Port is not available!\n");
    exit(0);
  }
  printf("Binding on Port: %d\n", PORT_NUMBER);
  int LISTEN_STATUS = listen(PROXY_SOCKET_ID, MAX_CLIENTS);

  if (LISTEN_STATUS < 0)
  {
    printf("Error occured while listening!\n");
    exit(1);
  }

  int ITERATOR = 0;
  int CONNECTED_SOCKET_ID[MAX_CLIENTS];

  while (1)
  {
    bzero((char *)&CLIENT_ADDR, sizeof(CLIENT_ADDR));
    CLIENT_LENGTH = sizeof(CLIENT_ADDR);
    CLIENT_SOCKET_ID = accept(PROXY_SOCKET_ID, (struct sockaddr *)&CLIENT_ADDR,
                              (socklen_t *)&CLIENT_LENGTH);

    if (CLIENT_SOCKET_ID < 0)
    {
      printf("Unable to connect new user!\n");
      exit(1);
    }
    else
    {
      CONNECTED_SOCKET_ID[ITERATOR] = CLIENT_SOCKET_ID;
    }

    struct sockaddr_in *CLIENT_PTR = (struct sockaddr_in *)&CLIENT_ADDR;
    struct in_addr IP_ADDR = CLIENT_PTR->sin_addr;
    char str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &IP_ADDR, str, INET_ADDRSTRLEN);
    printf("Client is connected via Port number: %d and IP address: %s\n",
           ntohs(CLIENT_ADDR.sin_port), str);

    pthread_create(&THREAD_ID[ITERATOR], NULL, THREAD_ROUTINE,
                   (void *)&CONNECTED_SOCKET_ID[ITERATOR]);
    ITERATOR += 1;
  }
  close(PROXY_SOCKET_ID);
  return 0;
}