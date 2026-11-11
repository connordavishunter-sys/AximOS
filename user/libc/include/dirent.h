#pragma once

#include <fs_types.h>

typedef struct DIR DIR;

DIR *opendir(const char *path);

// Returns the next entry, valid until the next call, or NULL after the last.
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);
void rewinddir(DIR *dir);
int dirfd(DIR *dir);

// Allocates a sorted list of what filter accepts, each entry to be freed along with
// the list, and returns how many. filter and compare may be NULL.
int scandir(const char *path, struct dirent ***list, int (*filter)(const struct dirent *),
            int (*compare)(const struct dirent **, const struct dirent **));
int alphasort(const struct dirent **a, const struct dirent **b);
