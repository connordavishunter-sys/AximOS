#pragma once

#include <net_types.h>

// What AximOS's libc has and glibc hasn't, for programs built for Linux.

#define GUI_HOSTED // on another system, whose computer isn't AximOS's to turn off

void help_option(int argc, char **argv, const char *text);

// The first network card with an IPv4 address, as far as Linux tells everyone.
int net_info(struct net_info *out);
int net_airplane(bool on);
