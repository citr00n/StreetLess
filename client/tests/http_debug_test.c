/* Host test: cc -std=c11 -Wall -Wextra -Werror -Iclient/include
 * client/source/http.c client/tests/http_debug_test.c -lcurl -o /tmp/streetless-http-test
 * /tmp/streetless-http-test */
#include "http.h"
#include <assert.h>
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void headers(int fd, char *request, size_t cap) {
    size_t used = 0;
    while (!strstr(request, "\r\n\r\n")) {
        assert(used + 1 < cap);
        ssize_t n = recv(fd, request + used, cap - used - 1, 0);
        assert(n > 0);
        used += (size_t)n;
        request[used] = 0;
    }
}

static void serve(int listener, int response_status, const char *reply, bool pair, bool succeeds) {
    struct pollfd ready = {listener, POLLIN, 0};
    assert(poll(&ready, 1, 5000) == 1);
    int fd = accept(listener, NULL, NULL);
    assert(fd >= 0);
    char request[4096] = {0};
    headers(fd, request, sizeof(request));
    assert(strstr(request, "GET /v1/debug HTTP/1.1\r\n"));
    assert(strstr(request, "X-Streetless-Debug-Pin: 0123\r\n"));
    assert(!strstr(request, "Authorization:"));
    assert(!strstr(request, "code="));
    char response[256];
    int len = snprintf(response, sizeof(response),
        "HTTP/1.1 %d Test\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
        response_status, strlen(reply), reply);
    assert(send(fd, response, (size_t)len, 0) == len);
    close(fd);
    if (!succeeds) {
        assert(poll(&ready, 1, 300) == 0);
        return;
    }
    assert(poll(&ready, 1, 5000) == 1);
    fd = accept(listener, NULL, NULL);
    assert(fd >= 0);
    memset(request, 0, sizeof(request));
    headers(fd, request, sizeof(request));
    assert(strstr(request, pair ? "POST /v1/pair HTTP/1.1\r\n" : "GET /v1/social HTTP/1.1\r\n"));
    assert(strstr(request, "X-Streetless-Debug-Pin: 0123\r\n"));
    if (pair) assert(!strstr(request, "Authorization:"));
    else assert(strstr(request, "Authorization: Bearer secret\r\n"));
    static const char ok[] = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
    assert(send(fd, ok, sizeof(ok) - 1, 0) == (ssize_t)(sizeof(ok) - 1));
    close(fd);
}

static void scenario(int response_status, const char *reply, bool pair, CURLcode expected) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(listener, 4) == 0);
    socklen_t len = sizeof(addr);
    assert(getsockname(listener, (struct sockaddr *)&addr, &len) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(listener);
        Config c = {.host = "localhost", .ipv4 = "127.0.0.1", .token = "secret", .port = ntohs(addr.sin_port)};
        Http h;
        assert(http_open(&h, &c, NULL, "0123"));
        char response[64];
        size_t received;
        long status;
        CURLcode rc = pair ? http_pair(&h, "code=secret&token=secret", 24, response, sizeof(response), &received, &status)
                           : http_request(&h, "/v1/social", NULL, NULL, 0, response, sizeof(response), &received, &status);
        assert(rc == expected);
        if (expected == CURLE_OK) assert(status == 204);
        http_close(&h);
        _exit(0);
    }
    serve(listener, response_status, reply, pair, expected == CURLE_OK);
    close(listener);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static bool cancelled(void *data) {
    return *(bool *)data;
}

static void cancel_before_request(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(listener, 1) == 0);
    socklen_t len = sizeof(addr);
    assert(getsockname(listener, (struct sockaddr *)&addr, &len) == 0);
    Config c = {.host = "localhost", .ipv4 = "127.0.0.1", .token = "secret", .port = ntohs(addr.sin_port)};
    Http h;
    assert(http_open(&h, &c, NULL, "0123"));
    bool stop = true;
    assert(http_set_cancel(&h, cancelled, &stop));
    char response[64];
    size_t received = 123;
    long status = 123;
    assert(http_request(&h, "/v1/social", NULL, NULL, 0, response, sizeof(response),
                        &received, &status) == CURLE_ABORTED_BY_CALLBACK);
    assert(received == 0 && status == 0);
    struct pollfd ready = {listener, POLLIN, 0};
    assert(poll(&ready, 1, 0) == 0);
    http_close(&h);
    close(listener);
}

int main(void) {
    assert(http_debug_pin_valid("0000") && http_debug_pin_valid("1234"));
    assert(!http_debug_pin_valid(NULL) && !http_debug_pin_valid("123") &&
           !http_debug_pin_valid("12345") && !http_debug_pin_valid("12a4"));
    cancel_before_request();
    scenario(200, "wrong", false, CURLE_WEIRD_SERVER_REPLY);
    scenario(200, "streetless-debug-v1\nextra", true, CURLE_WEIRD_SERVER_REPLY);
    scenario(401, "A long unauthorized response from a proxy", true, CURLE_LOGIN_DENIED);
    scenario(502, "A long error response from a proxy", false, CURLE_WEIRD_SERVER_REPLY);
    scenario(200, "streetless-debug-v1\n", false, CURLE_OK);
    scenario(200, "streetless-debug-v1\n", true, CURLE_OK);
    return 0;
}
