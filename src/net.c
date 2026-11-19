#include "net.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
int net_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
int net_listen(const char *host, int port)
{
    struct addrinfo hints, *list, *p;
    char service[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_PASSIVE;
    snprintf(service, sizeof(service), "%d", port);
    if (getaddrinfo(host, service, &hints, &list)) return -1;
    int fd = -1;
    for (p = list; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (!bind(fd, p->ai_addr, p->ai_addrlen) && !listen(fd, 64)) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(list);
    return fd;
}
int net_connect(const char *host, int port, int timeout_ms)
{
    struct addrinfo hints, *list, *p;
    char service[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%d", port);
    if (getaddrinfo(host, service, &hints, &list)) return -1;
    int fd = -1;
    for (p = list; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        net_set_nonblocking(fd);
        int rc = connect(fd, p->ai_addr, p->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            struct pollfd pf = { fd, POLLOUT, 0 };
            int err = 0; socklen_t len = sizeof(err);
            if (poll(&pf, 1, timeout_ms) == 1 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) && !err) rc = 0;
        }
        if (!rc) {
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
            break;
        }
        close(fd); fd = -1;
    }
    freeaddrinfo(list);
    return fd;
}
int net_send_all(int fd, const char *text, size_t length)
{
    while (length) {
        ssize_t k = send(fd, text, length, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        text += k; length -= (size_t)k;
    }
    return 0;
}
void reader_init(LineReader *reader, int fd)
{
    reader->fd = fd; reader->len = 0;
}
int reader_line(LineReader *reader, char *out, size_t capacity, int timeout_ms)
{
    for (;;) {
        char *nl = memchr(reader->buf, '\n', reader->len);
        if (nl) {
            size_t n = (size_t)(nl - reader->buf);
            size_t copy = n < capacity - 1 ? n : capacity - 1;
            memcpy(out, reader->buf, copy);
            if (copy && out[copy - 1] == '\r') --copy;
            out[copy] = 0;
            memmove(reader->buf, nl + 1, reader->len - n - 1);
            reader->len -= n + 1;
            return 1;
        }
        if (reader->len >= sizeof(reader->buf)) return -3;
        struct pollfd pf = { reader->fd, POLLIN, 0 };
        int rc = poll(&pf, 1, timeout_ms);
        if (rc < 0) return errno == EINTR ? -4 : -1;
        if (!rc) return -2;
        ssize_t k = recv(reader->fd, reader->buf + reader->len, sizeof(reader->buf) - reader->len, 0);
        if (k == 0) return 0;
        if (k < 0) {
            if (errno == EINTR) return -4;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        reader->len += (size_t)k;
    }
}
