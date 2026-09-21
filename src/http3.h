#ifndef HTTP3_H
#define HTTP3_H

void start_http3_server(int port);
void *http3_worker_thread(void *arg);

#endif /* HTTP3_H */
