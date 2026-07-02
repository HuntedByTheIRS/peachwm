/* Copyright and license details as described in the LICENSE file. */
#pragma once

#include <stddef.h>

[[noreturn]] void die(const char *fmt, ...);
[[nodiscard]] void *ecalloc(size_t nmemb, size_t size);
[[nodiscard]] int fd_set_nonblock(int fd);
