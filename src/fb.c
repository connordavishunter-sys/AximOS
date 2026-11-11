#include <console.h>
#include <errno_values.h>
#include <fb.h>
#include <io.h>
#include <limine.h>
#include <mm.h>
#include <serial.h>
#include <string.h>

// The bootloader leaves the screen in a mode that programs can draw on anywhere.
// Changing mode takes knowing the graphics card, and the one known here is the
// Bochs interface of QEMU's standard VGA, with the bootloader's as the only mode
// on anything else.

#define DISPI_INDEX 0x1CE
#define DISPI_DATA 0x1CF

#define DISPI_ID 0
#define DISPI_XRES 1
#define DISPI_YRES 2
#define DISPI_BPP 3
#define DISPI_ENABLE 4
#define DISPI_VIRT_WIDTH 6
#define DISPI_X_OFFSET 8
#define DISPI_Y_OFFSET 9
#define DISPI_VIDEO_MEMORY_64K 0xA

#define DISPI_ID_FIRST 0xB0C0
#define DISPI_ID_LAST 0xB0CF
#define DISPI_DISABLED 0x00
#define DISPI_ENABLED 0x01
#define DISPI_LFB_ENABLED 0x40
#define DISPI_NO_CLEAR 0x80

#define BYTES_PER_PIXEL 4
#define MIN_WIDTH 320
#define MIN_HEIGHT 200
#define MAX_SIDE 0xFFFF
#define KIB_64 0x10000

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request fb_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
};

static uint8_t *base;
static uint64_t phys_base;
static uint64_t memory_size;
static uint32_t width, height, pitch;
static bool bochs;
static bool xrgb;
static bool taken;

static uint16_t dispi_read(uint16_t index) {
    outw(DISPI_INDEX, index);
    return inw(DISPI_DATA);
}

static void dispi_write(uint16_t index, uint16_t value) {
    outw(DISPI_INDEX, index);
    outw(DISPI_DATA, value);
}

void fb_init(void) {
    struct limine_framebuffer_response *response = fb_request.response;

    if (!response || response->framebuffer_count == 0) {
        kprintf("fb: no framebuffer, output goes to serial only\n");
        return;
    }

    struct limine_framebuffer *fb = response->framebuffers[0];
    if (fb->bpp != 32) {
        kprintf("fb: unsupported %u bpp framebuffer\n", fb->bpp);
        return;
    }

    base = fb->address;
    width = fb->width;
    height = fb->height;
    pitch = fb->pitch;
    xrgb = fb->memory_model == LIMINE_FRAMEBUFFER_RGB && fb->red_mask_shift == 16 && fb->red_mask_size == 8
        && fb->green_mask_shift == 8 && fb->green_mask_size == 8 && fb->blue_mask_shift == 0
        && fb->blue_mask_size == 8;
    console_set_screen(base, width, height, pitch);
}

void fb_map(void) {
    if (!base)
        return;

    phys_base = mm_virt_to_phys(base);
    memory_size = (uint64_t)pitch * height;

    uint16_t id = dispi_read(DISPI_ID);

    bochs = id >= DISPI_ID_FIRST && id <= DISPI_ID_LAST;
    if (bochs && dispi_read(DISPI_VIDEO_MEMORY_64K) * (uint64_t)KIB_64 > memory_size)
        memory_size = dispi_read(DISPI_VIDEO_MEMORY_64K) * (uint64_t)KIB_64;

    // The bootloader only mapped what its mode uses, and may not have mapped that
    // for speed, so this is a mapping of everything that writes are quick through.
    uint8_t *mapped = mm_map_framebuffer(phys_base, memory_size);

    if (!mapped) {
        bochs = xrgb = false;
        kprintf("fb: not enough memory to map the screen\n");
        return;
    }
    base = mapped;
    console_set_base(base);
    kprintf("fb: %ux%u, %lu KiB of memory, %s\n", width, height, memory_size / 1024,
            bochs ? "resolution can change" : "resolution is fixed");
}

bool fb_drawable(void) {
    return base && xrgb;
}

int fb_acquire(void) {
    if (taken)
        return -EBUSY;

    taken = true;
    console_hide();
    return 0;
}

void fb_release(void) {
    taken = false;
    console_show();
}

void fb_get_info(struct fb_info *out) {
    *out = (struct fb_info){
        .width = width,
        .height = height,
        .pitch = pitch,
        .can_set_mode = bochs,
        .memory_size = memory_size,
    };
}

static void dispi_set(uint16_t x, uint16_t y, uint16_t enable) {
    dispi_write(DISPI_ENABLE, DISPI_DISABLED);
    dispi_write(DISPI_BPP, 8 * BYTES_PER_PIXEL);
    dispi_write(DISPI_XRES, x);
    dispi_write(DISPI_YRES, y);
    dispi_write(DISPI_X_OFFSET, 0);
    dispi_write(DISPI_Y_OFFSET, 0);
    dispi_write(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB_ENABLED | enable);
}

int fb_set_mode(uint64_t new_width, uint64_t new_height) {
    if (!bochs)
        return -ENOTSUP;
    if (new_width < MIN_WIDTH || new_height < MIN_HEIGHT || new_width > MAX_SIDE || new_height > MAX_SIDE)
        return -EINVAL;
    if (new_width * new_height * BYTES_PER_PIXEL > memory_size)
        return -EINVAL;

    dispi_set(new_width, new_height, 0);

    // The card rounds or caps what it can't do, which then isn't what was asked.
    if (dispi_read(DISPI_XRES) != new_width || dispi_read(DISPI_YRES) != new_height) {
        dispi_set(width, height, DISPI_NO_CLEAR);
        return -EINVAL;
    }

    width = new_width;
    height = new_height;
    pitch = dispi_read(DISPI_VIRT_WIDTH) * BYTES_PER_PIXEL;
    console_set_screen(base, width, height, pitch);
    return 0;
}

int fb_draw(const struct fb_rect *rect) {
    int64_t x = rect->x, y = rect->y, w = rect->width, h = rect->height;
    uint64_t stride = rect->stride ? rect->stride : (uint64_t)w * BYTES_PER_PIXEL;
    uint64_t pixels = (uint64_t)rect->pixels;

    if (w < 0 || h < 0 || stride < (uint64_t)w * BYTES_PER_PIXEL)
        return -EINVAL;

    // Trim what's off the screen, from the source pixels too.
    if (x < 0) {
        pixels += -x * BYTES_PER_PIXEL;
        w += x;
        x = 0;
    }
    if (y < 0) {
        pixels += -y * stride;
        h += y;
        y = 0;
    }
    if (x + w > width)
        w = (int64_t)width - x;
    if (y + h > height)
        h = (int64_t)height - y;
    if (w <= 0 || h <= 0)
        return 0;

    uint64_t row_bytes = w * BYTES_PER_PIXEL;

    if (!mm_user_range_ok(pixels, (h - 1) * stride + row_bytes, false))
        return -EFAULT;

    uint8_t *to = base + y * pitch + x * BYTES_PER_PIXEL;

    for (int64_t row = 0; row < h; row++)
        memcpy(to + row * pitch, (const void *)(pixels + row * stride), row_bytes);
    return 0;
}

uint64_t fb_memory_address(void) {
    return phys_base;
}

uint64_t fb_memory_size(void) {
    return memory_size;
}
