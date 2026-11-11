#pragma once

struct sysinfo {
    long uptime; // seconds since boot
    unsigned long loads[3]; // always 0
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram; // always 0
    unsigned long bufferram; // the disk cache
    unsigned long totalswap; // always 0
    unsigned long freeswap; // always 0
    unsigned short procs; // always 0
    unsigned long totalhigh; // always 0
    unsigned long freehigh; // always 0
    unsigned mem_unit;
};

int sysinfo(struct sysinfo *out);
int get_nprocs(void);
int get_nprocs_conf(void);
