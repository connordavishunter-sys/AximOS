#pragma once

#define PR_SET_NAME 15
#define PR_GET_NAME 16

// Nothing can be set this way, so it fails with EINVAL.
int prctl(int option, ...);
