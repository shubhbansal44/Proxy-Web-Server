#include "proxy_parse.h"
#include "config.h"
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
#include <ctype.h>
#include <time.h>
#include <unistd.h>

#include "cache.h"

// Global configuration structure
ProxyConfig g_config;

// Server's socket id
int PROXY_SOCKET_ID;

// Buffer to store Thread IDs associated with each client's request.
pthread_t *THREAD_ID;

// semaphore lock for handling multiple (MAX CLIENTS) users.
sem_t SEMAPHORE;


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
  char *BUFFER = (char *)malloc(g_config.max_bytes * sizeof(char));
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

  if (ParsedRequest_unparse_headers(CLIENT_PARSED_REQUEST, BUFFER + LENGTH, (size_t)g_config.max_bytes - LENGTH) < 0)
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
  char *RESPONSE = (char *)malloc(g_config.max_bytes);
  if (!RESPONSE)
  {
    close(END_SERVER_SOCKET_ID);
    return -1;
  }
  size_t RESPONSE_CAPACITY = g_config.max_bytes;
  size_t RESPONSE_LENGTH = 0;

  ssize_t BYTES_RECEIVED;
  while ((BYTES_RECEIVED = recv(END_SERVER_SOCKET_ID, BUFFER, g_config.max_bytes, 0)) > 0)
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

  char *BUFFER = (char *)calloc(g_config.max_bytes, sizeof(char));
  memset(BUFFER, 0, g_config.max_bytes);

  BYTES_RECIEVED = recv(SOCKET, BUFFER, g_config.max_bytes, 0);
  while (BYTES_RECIEVED > 0)
  {
    LENGTH = strlen(BUFFER);
    if (strstr(BUFFER, "\r\n\r\n") == NULL)
    {
      BYTES_RECIEVED = recv(SOCKET, BUFFER + LENGTH, g_config.max_bytes - LENGTH, 0);
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
    char RESPONSE[g_config.max_bytes];
    while (POS < SIZE)
    {
      memset(RESPONSE, 0, g_config.max_bytes);
      for (size_t i = 0; i < g_config.max_bytes; i++)
      {
        RESPONSE[i] = CACHE->DATA[POS];
        POS++;
      }
      send(SOCKET, RESPONSE, g_config.max_bytes, 0);
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
      memset(BUFFER, 0, g_config.max_bytes);
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


int main(int argc, char *argv[])
{

  // Initialize defaults
  config_init_defaults(&g_config);
  config_load_file(&g_config, "/etc/proxy/proxy.conf");
  config_apply_env(&g_config);
  
  int CLIENT_SOCKET_ID, CLIENT_LENGTH;
  struct sockaddr_in SERVER_ADDR, CLIENT_ADDR;
  sem_init(&SEMAPHORE, 0, g_config.max_clients);
  Cache_init();

  if (argc == 2 && (argv[1][0] == '-' || argv[1][0] == '/' || isdigit(argv[1][0])))
  {
    // Preserve positional fallback if numeric; also support --port=N
    if (strncmp(argv[1], "--port=", 7) == 0) {
      g_config.port = atoi(argv[1] + 7);
    } else if (strncmp(argv[1], "--max-clients=", 14) == 0) {
      g_config.max_clients = atoi(argv[1] + 14);
    } else if (strncmp(argv[1], "--cache-size=", 13) == 0) {
      g_config.max_cache_size = atol(argv[1] + 13);
    } else if (strncmp(argv[1], "--log-level=", 12) == 0) {
      strncpy(g_config.log_level, argv[1] + 12, sizeof(g_config.log_level)-1);
    } else {
      g_config.port = atoi(argv[1]); // positional
    }
  }
  
  // Pre-process positional numeric argument (backward compatibility)
  if (argc == 2 && isdigit(argv[1][0])) {
    g_config.port = atoi(argv[1]);
  }
  
  // Apply command-line overrides via official parser (handles --port=N etc.)
  // Skip parser for pure positional numeric to preserve backward compatibility
  int args_ok = 1;
  if (argc == 2 && isdigit(argv[1][0])) {
    args_ok = 1; // allow positional
  } else {
    args_ok = (config_apply_args(&g_config, argc, argv) == 0);
  }
  
  if (args_ok == 0) {
    fprintf(stderr, "Invalid command-line arguments\n");
    exit(1);
  }
  
  if (config_validate(&g_config) < 0) {
    fprintf(stderr, "Configuration validation failed\n");
    exit(1);
  }
  
  // Dynamic allocations based on loaded limits
  THREAD_ID = (pthread_t *)malloc(sizeof(pthread_t) * g_config.max_clients);

  printf("Starting Proxy Server at Port: %d...\n", g_config.port);

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

  memset((char *)&SERVER_ADDR, 0, sizeof(SERVER_ADDR));
  SERVER_ADDR.sin_family = AF_INET;
  SERVER_ADDR.sin_port = htons(g_config.port);
  SERVER_ADDR.sin_addr.s_addr = INADDR_ANY;

  if (bind(PROXY_SOCKET_ID, (struct sockaddr *)&SERVER_ADDR,
           sizeof(SERVER_ADDR)) < 0)
  {
    printf("Port is not available!\n");
    exit(0);
  }
  printf("Binding on Port: %d\n", g_config.port);
  int LISTEN_STATUS = listen(PROXY_SOCKET_ID, g_config.max_clients);

  if (LISTEN_STATUS < 0)
  {
    printf("Error occured while listening!\n");
    exit(1);
  }

  int ITERATOR = 0;
  int *CONNECTED_SOCKET_ID = (int *)malloc(sizeof(int) * g_config.max_clients);

  while (1)
  {
    memset((char *)&CLIENT_ADDR, 0, sizeof(CLIENT_ADDR));
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
  free(THREAD_ID);
  free(CONNECTED_SOCKET_ID);
  return 0;
}
