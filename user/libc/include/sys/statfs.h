#pragma once

#include <fs_types.h>

int statfs(const char *path, struct statfs *out);
