#pragma once

#define LOG_ERR 3
#define LOG_INFO 6
#define LOG_NOTICE 5
#define LOG_WARNING 4
#define LOG_DEBUG 7
#define LOG_PID 1
#define LOG_CONS 2
#define LOG_NOWAIT 0x10
#define LOG_AUTH (4 << 3)
#define LOG_USER (1 << 3)
#define LOG_DAEMON (3 << 3)
#define LOG_MAKEPRI(facility, priority) ((facility) | (priority))

// There's no system log, so these do nothing.
void openlog(const char *name, int options, int facility);
void syslog(int priority, const char *format, ...);
void closelog(void);
