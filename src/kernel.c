#include <acpi.h>
#include <ahci.h>
#include <ata.h>
#include <block.h>
#include <clock.h>
#include <fb.h>
#include <console.h>
#include <cpu.h>
#include <irq.h>
#include <kernel.h>
#include <keyboard.h>
#include <limine.h>
#include <mm.h>
#include <mouse.h>
#include <net.h>
#include <process.h>
#include <random.h>
#include <serial.h>
#include <stdint.h>
#include <vfs.h>

#define INIT_PATH "/system/boot/init"

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile uint64_t base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t requests_end[] = LIMINE_REQUESTS_END_MARKER;

void halt(void) {
    for (;;)
        __asm__ volatile("cli; hlt");
}

void die(const char *message) {
    cpu_stop_others();
    kprintf("error: %s, halting\n", message);
    halt();
}

void _start(void) {
    kprintf("kernel: started\n");

    if (!LIMINE_BASE_REVISION_SUPPORTED(base_revision))
        die("Limine base revision not supported");

    cpu_init_boot();
    fb_init();
    keyboard_init();

    bool have_mouse = mouse_init();

    if (!mm_init())
        die("bootloader did not provide a memory map");
    fb_map();
    if (!acpi_init())
        die("ACPI tables not usable");
    if (!irq_init())
        die("interrupt controllers not usable");
    clock_init();
    random_init();

    irq_enable_isa(IRQ_KEYBOARD);
    if (have_mouse) {
        irq_enable_isa(IRQ_MOUSE);
        vfs_add_device(VFS_DEVICE_MOUSE);
    }
    // A key pressed before now may be waiting, and its interrupt lost while masked.
    // The controller holds one byte at a time, so it would block both devices.
    keyboard_handle_irq();
    vfs_add_device(VFS_DEVICE_KEYBOARD);
    vfs_add_device(VFS_DEVICE_NULL);
    vfs_add_device(VFS_DEVICE_ZERO);
    vfs_add_device(VFS_DEVICE_TTY);
    vfs_add_device(VFS_DEVICE_PTMX);
    vfs_add_device(VFS_DEVICE_RANDOM);
    vfs_add_device(VFS_DEVICE_URANDOM);
    if (fb_drawable())
        vfs_add_device(VFS_DEVICE_FB);

    ata_init();
    ahci_init();
    if (block_device_count == 0)
        die("no drives found");
    if (!vfs_mount_all())
        die("no filesystems found");
    if (!vfs_choose_root(INIT_PATH))
        die(INIT_PATH " not found");

    struct vfs_node init;
    if (vfs_lookup("/", INIT_PATH, &init) != VFS_OK || process_spawn_init(&init, INIT_PATH) < 0)
        die("failed to load " INIT_PATH);

    net_init();
    cpu_start_others();
    process_idle_loop();
}
