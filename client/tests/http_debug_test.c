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

static void serve(int listener, const char *reply, bool pair, bool succeeds) {
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
        "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
        strlen(reply), reply);
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

static void scenario(const char *reply, bool pair, bool succeeds) {
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
        assert(succeeds ? rc == CURLE_OK && status == 204 : rc != CURLE_OK);
        http_close(&h);
        _exit(0);
    }
    serve(listener, reply, pair, succeeds);
    close(listener);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void) {
    assert(http_debug_pin_valid("0000") && http_debug_pin_valid("1234"));
    assert(!http_debug_pin_valid(NULL) && !http_debug_pin_valid("123") &&
           !http_debug_pin_valid("12345") && !http_debug_pin_valid("12a4"));
    scenario("wrong", false, false);
    scenario("streetless-debug-v1\nextra", true, false);
    scenario("streetless-debug-v1\n", false, true);
    scenario("streetless-debug-v1\n", true, true);
    return 0;
}
