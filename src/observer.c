#include "model.h"
#include "net.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void signal_handler(int signal_number) { interrupted = signal_number; }
static void out(const char *prefix, const char *text)
{
    char b[640];
    int n = snprintf(b, sizeof(b), "%s%s\n", prefix, text);
    if (n > 0 && write(STDOUT_FILENO, b, (size_t)n) < 0) return;
}
static int cli_number(const char *s, int low, int high)
{
    char *end; errno = 0;
    long n = strtol(s, &end, 10);
    return !*s || errno || *end || n < low || n > high ? -1 : (int)n;
}
static void usage(const char *name)
{
    printf("Usage: %s [--host ADDR] [--port N] [--name NAME] [--connect-timeout MS]\n"
           "Defaults: --host 127.0.0.1 --port 5320 --name observer\n", name);
}
int main(int argc, char **argv)
{
    const char *host = "127.0.0.1", *name = "observer";
    int port = 5320, connect_timeout = 3000;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        if (i + 1 >= argc) { usage(argv[0]); return 2; }
        const char *key = argv[i], *value = argv[++i];
        if (!strcmp(key, "--host")) host = value;
        else if (!strcmp(key, "--name")) {
            if (strlen(value) == 0 || strlen(value) > 40 || strpbrk(value, " \t\r\n")) return 2;
            name = value;
        }
        else if (!strcmp(key, "--port")) { port = cli_number(value, 1, 65535); if (port < 0) return 2; }
        else if (!strcmp(key, "--connect-timeout")) { connect_timeout = cli_number(value, 100, 600000); if (connect_timeout < 0) return 2; }
        else { usage(argv[0]); return 2; }
    }
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
    if (fd < 0) { out("", "observer: cannot connect to the kitchen server"); return interrupted ? 128 + interrupted : 3; }
    char request[96];
    snprintf(request, sizeof(request), "WATCH %s\n", name);
    if (net_send_all(fd, request, strlen(request))) { close(fd); return 3; }
    LineReader reader;
    reader_init(&reader, fd);
    int status = 0;
    for (;;) {
        if (interrupted) { status = 128 + interrupted; out("", "observer: disconnecting"); break; }
        char line[640];
        int rc = reader_line(&reader, line, sizeof(line), 5000);
        if (rc == -4) continue;
        if (rc == -2) { out("", "observer: no data from server for 5 s"); continue; }
        if (rc <= 0) { out("", "observer: connection to server lost"); status = 3; break; }
        if (!strncmp(line, "W ", 2)) out("== kitchen connected: ", line + 2);
        else if (!strncmp(line, "E ", 2)) out("", line + 2);
        else if (!strncmp(line, "S ", 2)) out("---- STATUS ", line + 2);
        else if (!strncmp(line, "X ", 2)) { out("== kitchen closed: ", line + 2); break; }
        else if (!strncmp(line, "ERR", 3)) { out("observer: rejected: ", line); status = 1; break; }
    }
    close(fd);
    return status;
}
