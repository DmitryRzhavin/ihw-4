#include "model.h"
#include "net.h"
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define IO_TIMEOUT_MS 15000
static const char *role_name = "";
static int actor_id;
static uint64_t started_at;
static void signal_handler(int signal_number) { interrupted = signal_number; }
/* Клиент выводит только то, что получил от сервера в ходе обмена. */
static void say(const char *format, ...)
{
    char b[512];
    int n = snprintf(b, sizeof(b), "[%6llu ms] %s %d: ", (unsigned long long)(now_ms() - started_at), role_name, actor_id);
    va_list ap; va_start(ap, format);
    int m = vsnprintf(b + n, sizeof(b) - (size_t)n, format, ap); va_end(ap);
    size_t len = (size_t)n + (size_t)(m > 0 ? m : 0);
    if (len > sizeof(b) - 2) len = sizeof(b) - 2;
    b[len++] = '\n';
    if (write(STDOUT_FILENO, b, len) < 0) return;
}
enum { RPC_OK = 0, RPC_SHUTDOWN = 1, RPC_LOST = -1, RPC_SIGNAL = -2 };
static int rpc(LineReader *reader, const char *request, Reply *out, char *reason, size_t reason_size)
{
    if (net_send_all(reader->fd, request, strlen(request))) return RPC_LOST;
    for (;;) {
        char line[256];
        int rc = reader_line(reader, line, sizeof(line), IO_TIMEOUT_MS);
        if (rc == -4) return RPC_SIGNAL;
        if (rc <= 0) return RPC_LOST;
        if (!strncmp(line, "X ", 2)) { snprintf(reason, reason_size, "%s", line + 2); return RPC_SHUTDOWN; }
        if (sscanf(line, "REP %d %d %d %d", &out->code, &out->value, &out->duration, &out->info) == 4) return RPC_OK;
        if (!strncmp(line, "ERR", 3)) { out->code = -2; return RPC_OK; }
    }
}
/* Ожидание приготовления с одновременным контролем сообщения о завершении сервера. */
static int cook_wait(LineReader *reader, int milliseconds, char *reason, size_t reason_size)
{
    uint64_t until = now_ms() + (uint64_t)milliseconds;
    while (now_ms() < until) {
        if (interrupted) return RPC_SIGNAL;
        int slice = (int)(until - now_ms());
        if (slice > 20) slice = 20;
        char line[256];
        int rc = reader_line(reader, line, sizeof(line), slice);
        if (rc == -2 || rc == -4) continue;
        if (rc <= 0) return RPC_LOST;
        if (!strncmp(line, "X ", 2)) { snprintf(reason, reason_size, "%s", line + 2); return RPC_SHUTDOWN; }
    }
    return RPC_OK;
}
static int cli_number(const char *s, int low, int high)
{
    char *end; errno = 0;
    long n = strtol(s, &end, 10);
    return !*s || errno || *end || n < low || n > high ? -1 : (int)n;
}
static void usage(const char *name)
{
    printf("Usage: %s --role visitor|waiter|cook --id N [--host ADDR] [--port N] [--connect-timeout MS]\n"
           "Defaults: --host 127.0.0.1 --port 5320 --connect-timeout 3000; ids start at 1\n", name);
}
int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    int port = 5320, role = -1, id = -1, connect_timeout = 3000;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        if (i + 1 >= argc) { usage(argv[0]); return 2; }
        const char *key = argv[i], *value = argv[++i];
        if (!strcmp(key, "--host")) host = value;
        else if (!strcmp(key, "--port")) { port = cli_number(value, 1, 65535); if (port < 0) return 2; }
        else if (!strcmp(key, "--id")) { id = cli_number(value, 1, MAX_VISITORS); if (id < 0) return 2; }
        else if (!strcmp(key, "--connect-timeout")) { connect_timeout = cli_number(value, 100, 600000); if (connect_timeout < 0) return 2; }
        else if (!strcmp(key, "--role")) {
            role = !strcmp(value, "visitor") ? VISITOR : !strcmp(value, "waiter") ? WAITER : !strcmp(value, "cook") ? COOK : -1;
            if (role < 0) return 2;
        } else { usage(argv[0]); return 2; }
    }
    if (role < 0 || id < 0) { usage(argv[0]); return 2; }
    role_name = role == VISITOR ? "visitor" : role == WAITER ? "waiter" : "cook";
    actor_id = id;
    started_at = now_ms();
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    int fd = -1;
    uint64_t give_up = now_ms() + (uint64_t)connect_timeout;
    while (fd < 0 && !interrupted) {
        fd = net_connect(host, port, 500);
        if (fd < 0) { if (now_ms() >= give_up) break; pause_ms(100); }
    }
    if (fd < 0) { say("cannot connect to %s:%d", host, port); return interrupted ? 128 + interrupted : 3; }
    LineReader reader;
    reader_init(&reader, fd);
    char reason[64] = "", request[96];
    Reply rep = {0, 0, 0, 0};
    snprintf(request, sizeof(request), "HELLO %s %d %d\n", role_name, id, (int)getpid());
    int rc = rpc(&reader, request, &rep, reason, sizeof(reason));
    if (rc != RPC_OK || rep.code < 0) {
        say(rc == RPC_OK ? "server refused connection (id busy or out of range)" : "connection lost during handshake");
        close(fd);
        return rc == RPC_OK ? 1 : 3;
    }
    say("connected to %s:%d", host, port);
    int status = 0, job = -1, arrived = role == VISITOR && rep.value == 1;
    uint64_t arrival = now_ms() + (uint64_t)rep.duration;
    if (role == VISITOR && arrived) say("returned to restaurant, order already placed");
    for (;;) {
        if (interrupted) { status = 128 + interrupted; say("interrupted, disconnecting"); break; }
        if (role == VISITOR) {
            if (!arrived && now_ms() >= arrival) {
                rc = rpc(&reader, "REQ 1 0\n", &rep, reason, sizeof(reason));
                if (rc == RPC_OK && rep.code >= 0) { arrived = 1; say("arrived and placed order"); }
            } else rc = rpc(&reader, "REQ 2 0\n", &rep, reason, sizeof(reason));
            if (rc == RPC_OK && rep.code == 2) {
                say(rep.value == 5 ? "received the order, leaving" : "order canceled, leaving without service");
                break;
            }
        } else if (role == WAITER) {
            rc = rpc(&reader, "REQ 3 0\n", &rep, reason, sizeof(reason));
            if (rc == RPC_OK && rep.value == 1) say("accepted order %d", rep.info);
            if (rc == RPC_OK && rep.value == 2) say("handed order %d to its visitor", rep.info);
            if (rc == RPC_OK && rep.value == 3) say("took over order %d from a disconnected waiter", rep.info);
            if (rc == RPC_OK && rep.code == 2) { say("all orders closed"); break; }
        } else if (job < 0) {
            rc = rpc(&reader, "REQ 4 0\n", &rep, reason, sizeof(reason));
            if (rc == RPC_OK && rep.code == 2) { say("no more work"); break; }
            if (rc == RPC_OK && rep.code == 1) {
                job = rep.value;
                say("took job %d for order %d, cooking %d ms", job, rep.info, rep.duration);
                rc = cook_wait(&reader, rep.duration, reason, sizeof(reason));
            }
        } else {
            snprintf(request, sizeof(request), "REQ 5 %d\n", job);
            rc = rpc(&reader, request, &rep, reason, sizeof(reason));
            if (rc == RPC_OK && rep.code == 1) { say("finished job %d", job); job = -1; }
        }
        if (rc == RPC_SHUTDOWN) { say("server finished: %s", reason); break; }
        if (rc == RPC_LOST) { say("connection to server lost"); status = 3; break; }
        if (rc == RPC_SIGNAL) continue;
        if (rc == RPC_OK && rep.code < 0) { say("server stopped or rejected the request (code %d)", rep.code); status = rep.code == -1 ? 0 : 1; break; }
        pause_ms(3);
    }
    if (status != 3 && rc != RPC_SHUTDOWN) {
        Reply ignored;
        char ignored_reason[16];
        rpc(&reader, "REQ 6 0\n", &ignored, ignored_reason, sizeof(ignored_reason));
    }
    close(fd);
    return status;
}
