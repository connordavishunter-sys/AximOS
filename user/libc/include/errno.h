#pragma once

#include <errno_values.h>

// Set by a failed call. Each thread has its own.
int *__errno_location(void);
#define errno (*__errno_location())
