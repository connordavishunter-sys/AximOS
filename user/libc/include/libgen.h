#pragma once

// Both may change path and may return a pointer into it or to a static string.
char *basename(char *path);
char *dirname(char *path);
