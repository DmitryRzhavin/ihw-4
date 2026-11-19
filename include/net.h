#ifndef NET_H
#define NET_H
#include <stddef.h>
typedef struct { int fd; char buf[2048]; size_t len; } LineReader;
int net_listen(const char *host, int port);
int net_connect(const char *host, int port, int timeout_ms);
int net_set_nonblocking(int fd);
int net_send_all(int fd, const char *text, size_t length);
void reader_init(LineReader *reader, int fd);
/* 1 - строка получена, 0 - соединение закрыто, -1 - ошибка, -2 - время вышло, -3 - строка слишком длинная, -4 - прервано сигналом */
int reader_line(LineReader *reader, char *out, size_t capacity, int timeout_ms);
#endif
