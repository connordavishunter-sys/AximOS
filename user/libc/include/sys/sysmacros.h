#pragma once

#define major(dev) ((unsigned)((dev) >> 8) & 0xFFF)
#define minor(dev) ((unsigned)(dev) & 0xFF)
#define makedev(major, minor) ((unsigned long)(major) << 8 | (minor))
