#ifndef ADMIN_H
#define ADMIN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int port;
    bool enabled;
} AdminConfig;

bool admin_server_start(const AdminConfig* config);
void admin_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif // ADMIN_H
