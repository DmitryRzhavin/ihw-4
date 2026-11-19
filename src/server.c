#include "model.h"
#include "net.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#define MAX_CONN 256
#define RING 100
#define GRACE_MS 2000
/* Соединение: участник кухни (после HELLO) либо наблюдатель (после WATCH). */
typedef struct {
    int fd, role, id, helloed, observer, closing;
    char name[64], in[1024], out[32768];
    size_t inlen, outlen;
} Conn;
static Conn conns[MAX_CONN];
static char ring[RING][512];
static int ring_start, ring_count, observer_count;
static Model model;
static void signal_handler(int signal_number) { interrupted = signal_number; }
static void queue_text(Conn *c, const char *text, size_t length)
{
    if (c->fd < 0 || c->closing) return;
    if (c->outlen + length > sizeof(c->out)) { c->closing = 2; return; }
    memcpy(c->out + c->outlen, text, length);
    c->outlen += length;
}
static void queue_format(Conn *c, const char *format, ...)
{
    char b[1024];
    va_list ap; va_start(ap, format);
    int n = vsnprintf(b, sizeof(b), format, ap); va_end(ap);
    if (n > 0) queue_text(c, b, (size_t)(n < (int)sizeof(b) ? n : (int)sizeof(b) - 1));
}
static void to_observers(const char *text, size_t length)
{
    for (int i = 0; i < MAX_CONN; ++i)
        if (conns[i].fd >= 0 && conns[i].observer) queue_text(&conns[i], text, length);
}
/* Каждое событие кухни попадает в журнал, на экран сервера и всем наблюдателям. */
static void on_event(const char *line, size_t length)
{
    int slot = (ring_start + ring_count) % RING;
    if (ring_count == RING) ring_start = (ring_start + 1) % RING; else ++ring_count;
    size_t n = length && line[length - 1] == '\n' ? length - 1 : length;
    if (n >= sizeof(ring[0])) n = sizeof(ring[0]) - 1;
    memcpy(ring[slot], line, n); ring[slot][n] = 0;
    char b[600];
    int k = snprintf(b, sizeof(b), "E %s\n", ring[slot]);
    to_observers(b, (size_t)k);
}
static void close_conn(Conn *c)
{
    if (c->fd < 0) return;
    if (c->observer) {
        --observer_count;
        event(&model, "OBSERVER_LEAVE name=%s observers=%d", c->name, observer_count);
    } else if (c->helloed) model_drop(&model, c->role, c->id);
    close(c->fd);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}
static void flush_conn(Conn *c)
{
    while (c->fd >= 0 && c->outlen) {
        ssize_t k = send(c->fd, c->out, c->outlen, MSG_NOSIGNAL);
        if (k > 0) { memmove(c->out, c->out + k, c->outlen - (size_t)k); c->outlen -= (size_t)k; continue; }
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        c->closing = 2;
        return;
    }
}
static void reply(Conn *c, Reply r)
{
    queue_format(c, "REP %d %d %d %d\n", r.code, r.value, r.duration, r.info);
}
static void handle_line(Conn *c, char *line)
{
    if (!c->helloed) {
        char word[16], name[64];
        int id = 0, pid = 0;
        if (sscanf(line, "HELLO %15s %d %d", word, &id, &pid) == 3) {
            int role = !strcmp(word, "visitor") ? VISITOR : !strcmp(word, "waiter") ? WAITER :
                !strcmp(word, "cook") ? COOK : -1;
            if (role < 0) { queue_format(c, "ERR role\n"); c->closing = 1; return; }
            Reply r = model_apply(&model, (Request){HELLO, role, id - 1, pid});
            reply(c, r);
            if (r.code < 0) { c->closing = 1; return; }
            c->helloed = 1; c->role = role; c->id = id - 1;
        } else if (sscanf(line, "WATCH %63s", name) == 1) {
            c->helloed = 1; c->observer = 1;
            snprintf(c->name, sizeof(c->name), "%s", name);
            ++observer_count;
            queue_format(c, "W visitors=%d waiters=%d cooks=%d unit_ms=%d\n",
                model.config.visitor_count, model.config.waiter_count, model.config.cook_count,
                model.config.unit_ms);
            for (int i = 0; i < ring_count; ++i)
                queue_format(c, "E %s\n", ring[(ring_start + i) % RING]);
            event(&model, "OBSERVER_JOIN name=%s observers=%d", c->name, observer_count);
        } else { queue_format(c, "ERR protocol\n"); c->closing = 1; }
        return;
    }
    if (c->observer) return;
    int action, value;
    if (sscanf(line, "REQ %d %d", &action, &value) != 2 || action <= HELLO || action > BYE) {
        queue_format(c, "ERR protocol\n"); c->closing = 1; return;
    }
    reply(c, model_apply(&model, (Request){action, c->role, c->id, value}));
    if (action == BYE) c->closing = 1;
}
static void read_conn(Conn *c)
{
    char tmp[512];
    ssize_t k = recv(c->fd, tmp, sizeof(tmp), 0);
    if (k == 0) { c->closing = 2; return; }
    if (k < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) c->closing = 2; return; }
    if (c->inlen + (size_t)k >= sizeof(c->in)) { c->closing = 2; return; }
    memcpy(c->in + c->inlen, tmp, (size_t)k);
    c->inlen += (size_t)k;
    char *nl;
    while (!c->closing && (nl = memchr(c->in, '\n', c->inlen)) != NULL) {
        size_t n = (size_t)(nl - c->in);
        *nl = 0;
        if (n && c->in[n - 1] == '\r') c->in[n - 1] = 0;
        handle_line(c, c->in);
        memmove(c->in, nl + 1, c->inlen - n - 1);
        c->inlen -= n + 1;
    }
}
static void status_line(void)
{
    int count[3] = {0, 0, 0}, running = 0;
    for (int r = 0; r < 3; ++r) for (int i = 0; i < MAX_VISITORS; ++i) count[r] += model.connected[r][i];
    for (int i = 0; i < MAX_WORKERS; ++i) running += model.cook_job[i] >= 0;
    char b[512];
    int n = snprintf(b, sizeof(b),
        "S t=%llu arrived=%d accepted=%d delivered=%d/%d canceled=%d queued=%d ready=%d cooking=%d visitors=%d waiters=%d cooks=%d observers=%d\n",
        (unsigned long long)(now_ms() - model.epoch_ms), model.arrived, model.accepted, model.delivered,
        model.config.visitor_count, model.canceled, model.queued, model.ready, running,
        count[0], count[1], count[2], observer_count);
    to_observers(b, (size_t)n);
}
static int participants(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CONN; ++i) n += conns[i].fd >= 0 && conns[i].helloed && !conns[i].observer;
    return n;
}
static int cli_number(const char *s, int low, int high)
{
    char *end; errno = 0;
    long n = strtol(s, &end, 10);
    return !*s || errno || *end || n < low || n > high ? -1 : (int)n;
}
static void usage(const char *name)
{
    printf("Usage: %s [--host ADDR] [--port 1..65535] [--config FILE] [--unit-ms 1..10000]\n"
           "          [--timeout 1..86400] [--log FILE|--no-log]\n"
           "Defaults: --host 127.0.0.1 --port 5320 --config configs/demo.conf --timeout 300\n", name);
}
int main(int argc, char **argv)
{
    const char *host = "127.0.0.1", *config_path = "configs/demo.conf", *log_path = "kitchen.log";
    int port = 5320, unit = 0, timeout = 300;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        if (!strcmp(argv[i], "--no-log")) { log_path = NULL; continue; }
        if (i + 1 >= argc) { usage(argv[0]); return 2; }
        const char *key = argv[i], *value = argv[++i];
        if (!strcmp(key, "--host")) host = value;
        else if (!strcmp(key, "--config")) config_path = value;
        else if (!strcmp(key, "--log")) log_path = value;
        else if (!strcmp(key, "--port")) { port = cli_number(value, 1, 65535); if (port < 0) return 2; }
        else if (!strcmp(key, "--unit-ms")) { unit = cli_number(value, 1, 10000); if (unit < 0) return 2; }
        else if (!strcmp(key, "--timeout")) { timeout = cli_number(value, 1, 86400); if (timeout < 0) return 2; }
        else { usage(argv[0]); return 2; }
    }
    Config config;
    if (config_load_file(config_path, &config)) return 2;
    if (unit) config.unit_ms = unit;
    for (int i = 0; i < MAX_CONN; ++i) conns[i].fd = -1;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    int listen_fd = net_listen(host, port);
    if (listen_fd < 0) { fprintf(stderr, "Cannot listen on %s:%d: %s\n", host, port, strerror(errno)); return 1; }
    net_set_nonblocking(listen_fd);
    if (log_path) {
        log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd < 0) { perror(log_path); close(listen_fd); return 1; }
    }
    model_init(&model, &config);
    event_hook = on_event;
    event(&model, "SERVER listening=%s:%d visitors=%d waiters=%d cooks=%d",
        host, port, config.visitor_count, config.waiter_count, config.cook_count);
    uint64_t deadline = now_ms() + (uint64_t)timeout * 1000, next_status = now_ms() + 1000, finished_at = 0;
    int status = 0;
    for (;;) {
        struct pollfd pfds[MAX_CONN + 1];
        int map[MAX_CONN + 1], n = 0;
        pfds[n] = (struct pollfd){ listen_fd, POLLIN, 0 }; map[n++] = -1;
        for (int i = 0; i < MAX_CONN; ++i)
            if (conns[i].fd >= 0) {
                pfds[n] = (struct pollfd){ conns[i].fd, (short)(POLLIN | (conns[i].outlen ? POLLOUT : 0)), 0 };
                map[n++] = i;
            }
        poll(pfds, (nfds_t)n, 5);
        if (interrupted) {
            model.stop = 1; model.signal_number = interrupted;
            status = interrupted == SIGINT ? 130 : 143;
            break;
        }
        if (now_ms() >= deadline) {
            model.stop = 1; model.signal_number = 1; status = 1;
            event(&model, "TIMEOUT seconds=%d", timeout);
            break;
        }
        for (;;) {
            int fd = accept(listen_fd, NULL, NULL);
            if (fd < 0) break;
            int slot = -1;
            for (int i = 0; i < MAX_CONN; ++i) if (conns[i].fd < 0) { slot = i; break; }
            if (slot < 0) { close(fd); continue; }
            net_set_nonblocking(fd);
            memset(&conns[slot], 0, sizeof(Conn));
            conns[slot].fd = fd;
        }
        for (int k = 1; k < n; ++k) {
            Conn *c = &conns[map[k]];
            if (c->fd >= 0 && (pfds[k].revents & (POLLIN | POLLHUP | POLLERR))) read_conn(c);
        }
        model_tick(&model);
        if (model_check(&model)) {
            event(&model, "INVARIANT_FAILURE");
            model.stop = 1; model.signal_number = 1; status = 1;
            break;
        }
        if (now_ms() >= next_status) { status_line(); next_status += 1000; }
        for (int i = 0; i < MAX_CONN; ++i) {
            Conn *c = &conns[i];
            if (c->fd < 0) continue;
            flush_conn(c);
            if (c->closing == 2 || (c->closing == 1 && !c->outlen)) close_conn(c);
        }
        if (model.finished == config.visitor_count) {
            if (!finished_at) finished_at = now_ms();
            if (!participants() || now_ms() > finished_at + GRACE_MS) break;
        }
    }
    model_summary(&model);
    const char *reason = status == 0 ? "normal" : status == 1 ? "failure" : "interrupted";
    for (int i = 0; i < MAX_CONN; ++i) {
        char b[64];
        int k = snprintf(b, sizeof(b), "X %s\n", reason);
        if (conns[i].fd >= 0) queue_text(&conns[i], b, (size_t)k);
    }
    uint64_t flush_until = now_ms() + 1000;
    while (now_ms() < flush_until) {
        int pending = 0;
        for (int i = 0; i < MAX_CONN; ++i) {
            if (conns[i].fd < 0) continue;
            flush_conn(&conns[i]);
            pending += conns[i].outlen != 0 && conns[i].closing != 2;
        }
        if (!pending) break;
        pause_ms(5);
    }
    event_hook = NULL;
    for (int i = 0; i < MAX_CONN; ++i) if (conns[i].fd >= 0) { close(conns[i].fd); conns[i].fd = -1; }
    close(listen_fd);
    if (log_fd >= 0) close(log_fd);
    return status;
}
