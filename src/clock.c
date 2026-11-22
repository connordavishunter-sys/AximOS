#include <clock.h>
#include <io.h>
#include <irq.h>
#include <kernel.h>
#include <serial.h>

#define CMOS_INDEX 0x70
#define CMOS_DATA 0x71

#define REG_SECOND 0x00
#define REG_MINUTE 0x02
#define REG_HOUR 0x04
#define REG_DAY 0x07
#define REG_MONTH 0x08
#define REG_YEAR 0x09
#define REG_STATUS_A 0x0A
#define REG_STATUS_B 0x0B

#define STATUS_A_UPDATING 0x80
#define STATUS_B_BINARY 0x04
#define STATUS_B_24_HOUR 0x02
#define HOUR_PM 0x80

#define CENTURY 2000
#define UPDATE_TIMEOUT 100000
#define SECONDS_PER_DAY 86400

static uint64_t tsc_hz;
static uint64_t tsc_start;
static uint64_t latest_ns;
static int64_t boot_epoch;

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

static void read_registers(uint8_t regs[6]) {
    for (int i = 0; i < UPDATE_TIMEOUT && (cmos_read(REG_STATUS_A) & STATUS_A_UPDATING); i++)
        ;

    regs[0] = cmos_read(REG_SECOND);
    regs[1] = cmos_read(REG_MINUTE);
    regs[2] = cmos_read(REG_HOUR);
    regs[3] = cmos_read(REG_DAY);
    regs[4] = cmos_read(REG_MONTH);
    regs[5] = cmos_read(REG_YEAR);
}

static int from_bcd(uint8_t value) {
    return (value >> 4) * 10 + (value & 0x0F);
}

void clock_init(void) {
    tsc_hz = irq_tsc_hz();
    if (!tsc_hz)
        die("can't tell how fast the timestamp counter runs");
    tsc_start = __builtin_ia32_rdtsc();
    boot_epoch = clock_now();
    kprintf("clock: timestamp counter runs at %lu Hz\n", tsc_hz);
}

uint64_t clock_monotonic_ns(void) {
    // Split up so that the multiplication can't overflow. CPUs' counters can be
    // slightly apart, and the kernel lock makes this check safe.
    uint64_t elapsed = __builtin_ia32_rdtsc() - tsc_start;
    uint64_t ns = elapsed / tsc_hz * NS_PER_SECOND + elapsed % tsc_hz * NS_PER_SECOND / tsc_hz;

    if (ns > latest_ns)
        latest_ns = ns;
    return latest_ns;
}

int64_t clock_boot_epoch(void) {
    return boot_epoch;
}

int64_t clock_now(void) {
    uint8_t regs[6], again[6];

    // The clock can tick between registers, so read until two reads agree.
    read_registers(again);
    do {
        for (int i = 0; i < 6; i++)
            regs[i] = again[i];
        read_registers(again);
    } while (regs[0] != again[0] || regs[1] != again[1] || regs[2] != again[2]
             || regs[3] != again[3] || regs[4] != again[4] || regs[5] != again[5]);

    uint8_t status = cmos_read(REG_STATUS_B);
    bool pm = !(status & STATUS_B_24_HOUR) && (regs[2] & HOUR_PM);

    regs[2] &= ~HOUR_PM;
    if (!(status & STATUS_B_BINARY)) {
        for (int i = 0; i < 6; i++)
            regs[i] = from_bcd(regs[i]);
    }

    struct clock_time time = {
        .year = CENTURY + regs[5],
        .month = regs[4],
        .day = regs[3],
        .hour = regs[2],
        .minute = regs[1],
        .second = regs[0],
    };

    if (!(status & STATUS_B_24_HOUR))
        time.hour = time.hour % 12 + (pm ? 12 : 0);

    return clock_to_epoch(&time);
}

// Howard Hinnant's civil calendar algorithms, which treat March as the start of
// the year so that leap days come last.
int64_t clock_to_epoch(const struct clock_time *t) {
    int year = t->month <= 2 ? t->year - 1 : t->year;
    int era = (year >= 0 ? year : year - 399) / 400;
    int year_of_era = year - era * 400;
    int shifted_month = t->month > 2 ? t->month - 3 : t->month + 9;
    int day_of_year = (153 * shifted_month + 2) / 5 + t->day - 1;
    int day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    int64_t days = (int64_t)era * 146097 + day_of_era - 719468;

    return days * SECONDS_PER_DAY + t->hour * 3600 + t->minute * 60 + t->second;
}

void clock_from_epoch(int64_t epoch, struct clock_time *t) {
    int64_t days = epoch / SECONDS_PER_DAY;
    int64_t rest = epoch % SECONDS_PER_DAY;

    if (rest < 0) {
        rest += SECONDS_PER_DAY;
        days--;
    }

    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int day_of_era = days - era * 146097;
    int year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    int day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    int shifted_month = (5 * day_of_year + 2) / 153;

    t->day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
    t->month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
    t->year = year_of_era + era * 400 + (t->month <= 2 ? 1 : 0);
    t->hour = rest / 3600;
    t->minute = rest % 3600 / 60;
    t->second = rest % 60;
}
