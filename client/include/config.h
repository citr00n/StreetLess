#ifndef STREETLESS_CONFIG_H
#define STREETLESS_CONFIG_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define STREETLESS_CONFIG_PATH "sdmc:/3ds/streetless/config.txt"
typedef struct {
    char host[254];
    char ipv4[16];
    char token[65];
    uint16_t port;
} Config;

bool config_read(FILE *input, Config *config);
bool config_endpoint_valid(const Config *config);
#endif
