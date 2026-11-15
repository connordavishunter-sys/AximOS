#include <acpi.h>
#include <block.h>
#include <hda.h>
#include <io.h>
#include <kernel.h>
#include <power.h>
#include <serial.h>
#include <vfs.h>
#include <stdint.h>

#define PS2_STATUS 0x64
#define PS2_COMMAND 0x64
#define PS2_INPUT_FULL 0x02
#define PS2_PULSE_RESET 0xFE
#define TIMEOUT 100000

// Waiting for the lock means no filesystem operation is left half written.
static void flush_disks(void) {
    vfs_lock();
    block_flush_all();
}

void power_off(void) {
    kprintf("kernel: powering off\n");
    hda_shutdown();
    flush_disks();
    acpi_power_off();
    die("power off failed");
}

void power_reboot(void) {
    kprintf("kernel: rebooting\n");
    hda_shutdown();
    flush_disks();
    acpi_reset();

    for (int i = 0; i < TIMEOUT && (inb(PS2_STATUS) & PS2_INPUT_FULL); i++)
        ;
    outb(PS2_COMMAND, PS2_PULSE_RESET);

    // If the keyboard controller didn't do it, an empty IDT forces a triple fault.
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) empty_idt = {0, 0};

    __asm__ volatile("lidt %0; int3" : : "m"(empty_idt));
    die("reboot failed");
}
