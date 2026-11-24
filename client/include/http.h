#ifndef STREETLESS_HTTP_H
#define STREETLESS_HTTP_H
#include "config.h"
#include <curl/curl.h>

typedef struct {
    CURL *easy;
    struct curl_slist *resolve;
    char origin[270];
    char authorization[88];
#ifdef __3DS__
    void *soc_buffer;
#endif
} Http;

bool http_open(Http *http, const Config *config, const char *ca_file);
void http_close(Http *http);
// A NULL MIME selects GET; otherwise request is POST. HTTP status is returned
// separately from transport status, even when the server sends an error body.
CURLcode http_request(Http *http, const char *path, const char *mime,
    const void *body, size_t len, void *response, size_t cap, size_t *received, long *status);
CURLcode http_pair(Http *http, const char *body, size_t len,
    void *response, size_t cap, size_t *received, long *status);
const char *http_error(CURLcode error);
#endif
