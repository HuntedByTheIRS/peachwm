/* Copyright and license details as described in the LICENSE file. */

#include <stddef.h>

[[noreturn]] void die(const char *fmt, ...);
void *ecalloc(size_t nmemb, size_t size);
int fd_set_nonblock(int fd);
