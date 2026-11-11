#pragma once

#include <netinet/in.h>

const char *inet_ntop(int family, const void *address, char *out, socklen_t size);
int inet_pton(int family, const char *text, void *out);
char *inet_ntoa(struct in_addr address);
in_addr_t inet_addr(const char *text);
int inet_aton(const char *text, struct in_addr *out);
