#include "proxy_parse.h"
#include "config.h"
#include "tls_tunnel.h"
#include "auth.h"
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
#include "logger.h"
#include "metrics.h"
#include "admin.h"
#include "http3.h"
#include <signal.h>

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
    LOG_ERROR("ConnectEndServer: Something went wrong while inializing end server socket!\n");
    return -1;
  }

  struct hostent *HOST = gethostbyname((const char *)hostname);
  if (HOST == NULL)
  {
    LOG_ERROR("ConnectEndServer: No such host exists: %s\n", (char *)hostname);
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
    LOG_ERROR("ConnectEndServer: connect to end server failed");
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
    LOG_ERROR("HandleRequest: Error occured While Establising Parsed request connection!\n");
  }

  struct ParsedHeader *h_hdr = ParsedHeader_get(CLIENT_PARSED_REQUEST, "Host");
  if (CLIENT_PARSED_REQUEST->host != NULL)
  {
    if (h_hdr == NULL || strstr(h_hdr->value, "localhost") != NULL || strstr(h_hdr->value, "127.0.0.1") != NULL)
    {
      if (ParsedHeader_set(CLIENT_PARSED_REQUEST, "Host", CLIENT_PARSED_REQUEST->host) < 0)
      {
        LOG_ERROR("HandleRequest: Error occured while setting host in Parsed Request header!\n");
      }
    }
  }

  if (ParsedRequest_unparse_headers(CLIENT_PARSED_REQUEST, BUFFER + LENGTH, (size_t)g_config.max_bytes - LENGTH) < 0)
  {
    LOG_ERROR("HandleRequest: Error occured while unparsing headers!\n");
  }

  int END_SERVER_PORT = 80;
  if (CLIENT_PARSED_REQUEST->port != NULL)
  {
    END_SERVER_PORT = atoi(CLIENT_PARSED_REQUEST->port);
  }

  int END_SERVER_SOCKET_ID = ConnectEndServer(CLIENT_PARSED_REQUEST->host, END_SERVER_PORT);
  if (END_SERVER_SOCKET_ID < 0)
  {
    LOG_ERROR("HandleRequest: Error occured while creating end server socket ID!\n");
    return -1;
  }

  /* send request to end server */
  ssize_t BYTES_SEND = send(END_SERVER_SOCKET_ID, BUFFER, strlen(BUFFER), 0);
  if (BYTES_SEND < 0)
  {
    LOG_ERROR("HandleRequest: sending request to end server failed");
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
      LOG_ERROR("HandleRequest: send to client failed");
      break;
    }
    /* append to RESPONSE buffer */
    if (RESPONSE_LENGTH + (size_t)BYTES_RECEIVED + 1 > RESPONSE_CAPACITY)
    {
      RESPONSE_CAPACITY *= 2;
      char *TEMP = (char *)realloc(RESPONSE, RESPONSE_CAPACITY);
      if (!TEMP)
      {
        LOG_ERROR("HandleRequest: realloc failed");
        break;
      }
      RESPONSE = TEMP;
    }
    memcpy(RESPONSE + RESPONSE_LENGTH, BUFFER, BYTES_RECEIVED);
    RESPONSE_LENGTH += BYTES_RECEIVED;
  }

  if (BYTES_RECEIVED < 0)
    LOG_ERROR("HandleRequest: recv from end server failed");

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
    LOG_INFO("400 Bad Request");
    send(socket, str, strlen(str), 0);
    break;

  case 403:
    snprintf(str, sizeof(str), "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>", currentTime);
    LOG_INFO("403 Forbidden");
    send(socket, str, strlen(str), 0);
    break;

  case 404:
    snprintf(str, sizeof(str), "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>", currentTime);
    LOG_INFO("404 Not Found");
    send(socket, str, strlen(str), 0);
    break;

  case 407:
    snprintf(str, sizeof(str), "HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm=\"Proxy\"\r\nContent-Length: 122\r\nContent-Type: text/html\r\nConnection: close\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>407 Proxy Authentication Required</TITLE></HEAD>\n<BODY><H1>407 Authentication Required</H1>\n</BODY></HTML>", currentTime);
    LOG_INFO("407 Proxy Authentication Required");
    send(socket, str, strlen(str), 0);
    break;

  case 500:
    snprintf(str, sizeof(str), "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>", currentTime);
    LOG_INFO("500 Internal Server Error");
    send(socket, str, strlen(str), 0);
    break;

  case 501:
    snprintf(str, sizeof(str), "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>", currentTime);
    LOG_INFO("501 Not Implemented");
    send(socket, str, strlen(str), 0);
    break;

  case 505:
    snprintf(str, sizeof(str), "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>", currentTime);
    LOG_INFO("505 HTTP Version Not Supported");
    send(socket, str, strlen(str), 0);
    break;

  default:
    return -1;
  }
  return 1;
}

void *THREAD_ROUTINE(void *NEW_SOCKET)
{
  struct timespec start_time, end_time;
  clock_gettime(CLOCK_MONOTONIC, &start_time);
  
  metrics_increment_requests();

  sem_wait(&SEMAPHORE);
  int CURRENT_SEMAPHORE_VALUE;
  sem_getvalue(&SEMAPHORE, &CURRENT_SEMAPHORE_VALUE);
  LOG_INFO("Currently available Sockets: %d", CURRENT_SEMAPHORE_VALUE);

  int *NEW_SOCKET_PTR = (int *)NEW_SOCKET;
  int SOCKET = *NEW_SOCKET_PTR;
  int BYTES_RECIEVED, LENGTH;

  char *BUFFER = (char *)calloc(g_config.max_bytes, sizeof(char));
  memset(BUFFER, 0, g_config.max_bytes);

  // Check if it's an HTTP/2 or WebSockets or generic TLS tunnel request (for port 443 proxy)
  // Or if it's h2c preface: "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
  const char *H2_PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
  
  BYTES_RECIEVED = recv(SOCKET, BUFFER, g_config.max_bytes, MSG_PEEK);
  
  if (BYTES_RECIEVED > 0 && 
      (BUFFER[0] == 0x16 || // TLS Handshake
       (BYTES_RECIEVED >= 24 && memcmp(BUFFER, H2_PREFACE, 24) == 0))) 
  {
      // Explicit CONNECT is mandatory. Raw TLS/h2c detected directly on a 
      // fresh socket instead of an HTTP/1.1 proxy payload must be rejected.
      LOG_ERROR("Raw TLS or h2c detected without prior CONNECT. Rejecting.");
      ThrowError(SOCKET, 400);
      
      // Get client IP for access log reporting before exit
      struct sockaddr_in client_addr_fail;
      socklen_t addr_len_fail = sizeof(client_addr_fail);
      char client_ip_fail[INET_ADDRSTRLEN] = "UNKNOWN";
      if (getpeername(SOCKET, (struct sockaddr*)&client_addr_fail, &addr_len_fail) == 0) {
          inet_ntop(AF_INET, &client_addr_fail.sin_addr, client_ip_fail, INET_ADDRSTRLEN);
      }
      
      clock_gettime(CLOCK_MONOTONIC, &end_time);
      double elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 + (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;
      logger_access_log(client_ip_fail, 400, 0, "UNKNOWN", "UNKNOWN", elapsed_ms, "text/html", "UNKNOWN");
      
      // Also grab the peek bytes to drain them
      recv(SOCKET, BUFFER, g_config.max_bytes, 0);
      
      close(SOCKET);
      free(BUFFER);
      sem_post(&SEMAPHORE);
      
      return NULL;
  }
  else
  {
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
  }

  char *REQUEST = (char *)malloc(strlen(BUFFER) * sizeof(char) + 1);
  for (size_t i = 0; i < strlen(BUFFER); i++)
  {
    REQUEST[i] = BUFFER[i];
  }

  // Get client IP for access log
  struct sockaddr_in client_addr;
  socklen_t addr_len = sizeof(client_addr);
  char client_ip[INET_ADDRSTRLEN] = "UNKNOWN";
  if (getpeername(SOCKET, (struct sockaddr*)&client_addr, &addr_len) == 0) {
      inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
  }

  int response_status = 200;
  int response_size = 0;
  const char* method = "UNKNOWN";
  const char* url = "UNKNOWN";
  char trace_id_str[64];
  snprintf(trace_id_str, sizeof(trace_id_str), "%08x%08x", (unsigned int)time(NULL), (unsigned int)rand());

  struct ParsedRequest *PARSED_REQUEST = ParsedRequest_create();
  int parse_result = -1;
  if (BYTES_RECIEVED > 0) {
      parse_result = ParsedRequest_parse(PARSED_REQUEST, BUFFER, strlen(BUFFER));
      if (parse_result >= 0) {
          struct ParsedHeader *trace_hdr = ParsedHeader_get(PARSED_REQUEST, "X-Trace-Id");
          if (trace_hdr && trace_hdr->value) {
              snprintf(trace_id_str, sizeof(trace_id_str), "%s", trace_hdr->value);
          } else {
              ParsedHeader_set(PARSED_REQUEST, "X-Trace-Id", trace_id_str);
          }
          if (PARSED_REQUEST->method) method = PARSED_REQUEST->method;
          if (PARSED_REQUEST->path) url = PARSED_REQUEST->path;
      }
  }

  CacheModule *CACHE = FindCache(REQUEST);
  if (CACHE != NULL)
  {
    metrics_increment_cache_hits();
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
        response_size++;
      }
      send(SOCKET, RESPONSE, g_config.max_bytes, 0);
    }
    LOG_INFO("Data retrived from cache");
    LOG_INFO("%s\n", RESPONSE);
    
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 + (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;
    logger_access_log(client_ip, 200, response_size, method, url, elapsed_ms, "text/html", trace_id_str);
    
    ParsedRequest_destroy(PARSED_REQUEST);
  }
  else if (BYTES_RECIEVED > 0)
  {
    LENGTH = strlen(BUFFER);

    if (parse_result < 0)
    {
      if (strncmp(BUFFER, "GET /", 5) == 0 && strncmp(BUFFER, "GET /http", 9) != 0) {
        // Deliberate drop for incomplete relative paths (like /favicon.ico) so they do not emit 400 error logs or crash.
        close(SOCKET);
        free(BUFFER);
        free(REQUEST);
        if (PARSED_REQUEST) ParsedRequest_destroy(PARSED_REQUEST);
        sem_post(&SEMAPHORE);
        return NULL;
      }
      LOG_INFO("Parsing failed!");
      ThrowError(SOCKET, 400);
      response_status = 400;
    }
    else
    {

      // Check auth if enabled
      if (g_config.enable_auth) {
        struct ParsedHeader *auth_hdr = ParsedHeader_get(PARSED_REQUEST, "Proxy-Authorization");
        const char *auth_val = auth_hdr ? auth_hdr->value : NULL;
        
        if (!check_basic_auth(auth_val)) {
            ThrowError(SOCKET, 407);
            response_status = 407;
            
            clock_gettime(CLOCK_MONOTONIC, &end_time);
            double elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 + (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;
            logger_access_log(client_ip, response_status, 0, method, url, elapsed_ms, "text/html", trace_id_str);
            
            ParsedRequest_destroy(PARSED_REQUEST);
            free(BUFFER);
            free(REQUEST);
            close(SOCKET);
            sem_post(&SEMAPHORE);
            return NULL;
        }
      }

      memset(BUFFER, 0, g_config.max_bytes);
if (!strcmp(PARSED_REQUEST->method, "GET"))
      {
        if (PARSED_REQUEST->host && PARSED_REQUEST->path && checkHTTPversion(PARSED_REQUEST->version) == 1)
        {
          int handle_res = HandleRequest(SOCKET, PARSED_REQUEST, REQUEST);
          if (handle_res == -1)
          {
            ThrowError(SOCKET, 500);
            response_status = 500;
            metrics_increment_errors();
          }
        }
        else
        {
          ThrowError(SOCKET, 500);
          response_status = 500;
          metrics_increment_errors();
        }
      }
      else if (!strcmp(PARSED_REQUEST->method, "CONNECT"))
      {
        int port = PARSED_REQUEST->port ? atoi(PARSED_REQUEST->port) : 443;
        int handle_res = HandleConnect(SOCKET, PARSED_REQUEST->host, port);
        if (handle_res == -1)
        {
          ThrowError(SOCKET, 500); // Bad Gateway or internal error
          response_status = 502;
          metrics_increment_errors();
        }
      }
      else
      {
        LOG_INFO("Can't handle request other than \'GET\'");
        ThrowError(SOCKET, 501);
        response_status = 501;
        metrics_increment_errors();
      }
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 + (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;
    logger_access_log(client_ip, response_status, 0, method, url, elapsed_ms, "text/html", trace_id_str); // Need actual sizes if possible
    ParsedRequest_destroy(PARSED_REQUEST);
  }
  else if (BYTES_RECIEVED == 0)
  {
    LOG_INFO("Request didn't received, user may be disconnected");
  }
  shutdown(SOCKET, SHUT_RDWR);
  close(SOCKET);
  free(BUFFER);

  sem_post(&SEMAPHORE);
  sem_getvalue(&SEMAPHORE, &CURRENT_SEMAPHORE_VALUE);
  LOG_INFO("Currently available Sockets: %d", CURRENT_SEMAPHORE_VALUE);

  free(REQUEST);
  
  return NULL;
}


void handle_signal(int sig) {
    LOG_INFO("Received signal %d, shutting down...", sig);
    admin_server_stop();
    CleanupOpenSSL();
    logger_shutdown();
    exit(0);
}

int main(int argc, char *argv[])
{
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);

  const char *config_file = getenv("PROXY_CONFIG_FILE");
  
  // Scan argv for --config=
  for (int i = 1; i < argc; i++) {
      if (strncmp(argv[i], "--config=", 9) == 0) {
          config_file = argv[i] + 9;
          break;
      }
  }

  if (!config_file) {
      if (access("/etc/proxy/proxy.conf", F_OK) == 0) {
          config_file = "/etc/proxy/proxy.conf";
      } else {
          config_file = "proxy.conf";
      }
  }

  // Initialize defaults
  InitOpenSSL();
  config_init_defaults(&g_config);
  config_load_file(&g_config, config_file);
  config_apply_env(&g_config);
  
  // Start hot reload
  config_start_hot_reload(config_file);
  
  int CLIENT_SOCKET_ID, CLIENT_LENGTH;
  struct sockaddr_in SERVER_ADDR, CLIENT_ADDR;
  sem_init(&SEMAPHORE, 0, g_config.max_clients);
  Cache_init();

  if (argc == 2 && isdigit(argv[1][0]))
  {
    g_config.port = atoi(argv[1]); // positional
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
    LOG_ERROR("Invalid command-line arguments\n");
    exit(1);
  }
  
  if (config_validate(&g_config) < 0) {
    LOG_ERROR("Configuration validation failed\n");
    exit(1);
  }
  
  // Initialize Auth & ACL subsystem
  auth_init(&g_config);
  
  // Initialize Observability subsystem
  LoggerConfig log_cfg;
  strncpy(log_cfg.log_file, g_config.log_file, sizeof(log_cfg.log_file)-1);
  log_cfg.log_file[sizeof(log_cfg.log_file)-1] = '\0';
  
  LogLevelEnum l_enum = LOG_LEVEL_INFO;
  if (!strcmp(g_config.log_level, "DEBUG")) l_enum = LOG_LEVEL_DEBUG;
  else if (!strcmp(g_config.log_level, "WARN")) l_enum = LOG_LEVEL_WARN;
  else if (!strcmp(g_config.log_level, "ERROR")) l_enum = LOG_LEVEL_ERROR;
  else if (!strcmp(g_config.log_level, "CRITICAL")) l_enum = LOG_LEVEL_CRITICAL;
  
  log_cfg.level = l_enum;
  log_cfg.max_file_size_mb = g_config.log_max_size_mb;
  log_cfg.rotation_enabled = g_config.log_rotation;
  log_cfg.use_syslog = false;
  if (strcmp(g_config.log_format, "JSON") == 0) log_cfg.format = LOG_FORMAT_JSON;
  else if (strcmp(g_config.log_format, "CLF") == 0) log_cfg.format = LOG_FORMAT_CLF;
  else log_cfg.format = LOG_FORMAT_SQUID;
  
  if (!logger_init(&log_cfg)) {
      LOG_ERROR("Failed to initialize logger!\n");
  }

  metrics_init();
  
  AdminConfig admin_cfg;
  admin_cfg.enabled = g_config.enable_admin;
  admin_cfg.port = g_config.admin_port;
  admin_server_start(&admin_cfg);
  
  // Dynamic allocations based on loaded limits
  THREAD_ID = (pthread_t *)malloc(sizeof(pthread_t) * g_config.max_clients);

  LOG_INFO("Starting Proxy Server at Port: %d...", g_config.port);

  start_http3_server(g_config.port);

  PROXY_SOCKET_ID = socket(AF_INET, SOCK_STREAM, 0);
  if (PROXY_SOCKET_ID < 0)
  {
    LOG_INFO("Failed to create Proxy Socket ID!");
    exit(1);
  }

  int REUSE = 1;
  if (setsockopt(PROXY_SOCKET_ID, SOL_SOCKET, SO_REUSEADDR,
                 (const char *)&REUSE, sizeof(REUSE)) < 0)
  {
    LOG_INFO("Execution failed while setting Socket option(setsockopt)!");
  }

  memset((char *)&SERVER_ADDR, 0, sizeof(SERVER_ADDR));
  SERVER_ADDR.sin_family = AF_INET;
  SERVER_ADDR.sin_port = htons(g_config.port);
  SERVER_ADDR.sin_addr.s_addr = INADDR_ANY;

  if (bind(PROXY_SOCKET_ID, (struct sockaddr *)&SERVER_ADDR,
           sizeof(SERVER_ADDR)) < 0)
  {
    LOG_INFO("Port is not available!");
    exit(0);
  }
  LOG_INFO("Binding on Port: %d", g_config.port);
  int LISTEN_STATUS = listen(PROXY_SOCKET_ID, g_config.max_clients);

  if (LISTEN_STATUS < 0)
  {
    LOG_INFO("Error occured while listening!");
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
      LOG_INFO("Unable to connect new user!");
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
    
    if (!check_ip_allowed(str)) {
        LOG_INFO("Connection denied for IP: %s (IP ACL block)", str);
        close(CLIENT_SOCKET_ID);
        CONNECTED_SOCKET_ID[ITERATOR] = 0;
        continue;
    }
    
    printf("Client is connected via Port number: %d and IP address: %s\n",
           ntohs(CLIENT_ADDR.sin_port), str);

    pthread_create(&THREAD_ID[ITERATOR], NULL, THREAD_ROUTINE,
                   (void *)&CONNECTED_SOCKET_ID[ITERATOR]);
    ITERATOR += 1;
  }
  close(PROXY_SOCKET_ID);
  CleanupOpenSSL();
  free(THREAD_ID);
  free(CONNECTED_SOCKET_ID);
  return 0;
}
