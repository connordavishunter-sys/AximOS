#include <hda.h>
#include <mm.h>
#include <pci.h>
#include <serial.h>
#include <string.h>

// The controller's registers.
#define GCAP 0x00
#define GCTL 0x08
#define STATESTS 0x0E
#define INTCTL 0x20
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP 0x48
#define CORBRP 0x4A
#define CORBCTL 0x4C
#define CORBSIZE 0x4E
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP 0x58
#define RINTCNT 0x5A
#define RIRBCTL 0x5C
#define RIRBSTS 0x5D
#define RIRBSIZE 0x5E
#define STREAMS 0x80 // each stream's registers, 0x20 apart, input streams first
#define REGISTERS_SIZE 0x4000

#define GCTL_RUNNING 0x01
#define RING_RESET 0x8000 // in CORBRP and RIRBWP
#define DMA_RUN 0x02      // in CORBCTL and RIRBCTL
#define RING_ENTRIES 256
#define RING_SIZE_256 0x02

// A stream's registers, from its base.
#define SD_CTL 0x00
#define SD_STS 0x03
#define SD_LPIB 0x04
#define SD_CBL 0x08
#define SD_LVI 0x0C
#define SD_FMT 0x12
#define SD_BDPL 0x18
#define SD_BDPU 0x1C
#define SD_RESET 0x01
#define SD_RUN 0x02
#define STREAM_TAG 1

// Verbs: 12 bits with an 8-bit payload, or 4 with a 16-bit one.
#define GET_PARAMETER 0xF00
#define GET_CONNECTIONS 0xF02
#define SET_SELECT 0x701
#define SET_POWER 0x705
#define SET_STREAM 0x706
#define SET_PIN 0x707
#define SET_EAPD 0x70C
#define GET_CONFIG 0xF1C
#define SET_FORMAT 0x2
#define SET_AMP 0x3

#define PARAM_NODES 0x04
#define PARAM_FUNCTION 0x05
#define PARAM_WIDGET 0x09
#define PARAM_PIN 0x0C
#define PARAM_CONNECTIONS 0x0E
#define PARAM_IN_AMP 0x0D
#define PARAM_OUT_AMP 0x12

#define FUNCTION_AUDIO 0x01
#define WIDGET_DAC 0x0
#define WIDGET_PIN 0x4
#define WIDGET_OUT_AMP 0x04
#define WIDGET_IN_AMP 0x02
#define PIN_OUTPUT 0x10
#define PIN_EAPD 0x10000
#define PIN_OUT_ENABLE 0x40
#define PIN_HEADPHONE 0x80
#define CONFIG_NOTHING 1 // what the port connects to, in bits 31 and 30
#define DEVICE_HEADPHONE 2
#define AMP_OUT 0x8000
#define AMP_IN 0x4000
#define AMP_BOTH 0x3000
#define AMP_MUTE 0x80
#define FORMAT_16_BIT 0x10
#define FORMAT_STEREO 0x01

#define MAX_NODES 128
#define MAX_PATH 6
#define MAX_PATHS 4

// The buffer the controller plays from, round and round: a page for each of its
// entries, which it moves on to in turn.
#define PAGES (HDA_BUFFER_BYTES / PAGE_SIZE)
#define TIMEOUT 1000000

struct bdl_entry {
    uint64_t address;
    uint32_t length;
    uint32_t flags;
};

static volatile uint8_t *registers;
static volatile uint32_t *corb;
static volatile uint64_t *rirb;
static uint16_t rirb_read;
static int codec, stream; // the codec's address, and the output stream's registers
static uint8_t *pages[PAGES];
static uint16_t format;


// The widgets from a pin back to a converter, which are set up to play.
static uint8_t paths[MAX_PATHS][MAX_PATH], inputs[MAX_PATHS][MAX_PATH]; // and which input each takes
static int path_lengths[MAX_PATHS], path_count;

static uint8_t r8(int r) { return registers[r]; }
static uint16_t r16(int r) { return *(volatile uint16_t *)(registers + r); }
static uint32_t r32(int r) { return *(volatile uint32_t *)(registers + r); }
static void w8(int r, uint8_t v) { registers[r] = v; }
static void w16(int r, uint16_t v) { *(volatile uint16_t *)(registers + r) = v; }
static void w32(int r, uint32_t v) { *(volatile uint32_t *)(registers + r) = v; }

static bool wait_for(int r, uint32_t mask, uint32_t want, int width) {
    for (int i = 0; i < TIMEOUT; i++) {
        uint32_t v = width == 8 ? r8(r) : width == 16 ? r16(r) : r32(r);

        if ((v & mask) == want)
            return true;
        __asm__ volatile("pause");
    }
    return false;
}

// Sends a verb to a node and waits for its answer, or 0 after too long.
static uint32_t command(int node, uint32_t verb, uint32_t payload) {
    uint16_t at = (r16(CORBWP) + 1) % RING_ENTRIES;
    uint32_t data = verb > 0xF ? verb << 8 | (payload & 0xFF) : verb << 16 | (payload & 0xFFFF);

    corb[at] = (uint32_t)codec << 28 | (uint32_t)node << 20 | data;
    w16(CORBWP, at);
    for (int i = 0; i < TIMEOUT; i++) {
        if ((r16(RIRBWP) & 0xFF) != rirb_read) {
            rirb_read = (rirb_read + 1) % RING_ENTRIES;
            w8(RIRBSTS, 0x05); // some controllers send no more until told this was seen
            return (uint32_t)rirb[rirb_read];
        }
        __asm__ volatile("pause");
    }
    return 0;
}

static uint32_t parameter(int node, int which) {
    return command(node, GET_PARAMETER, which);
}

static int widget_type(int node) {
    return parameter(node, PARAM_WIDGET) >> 20 & 0xF;
}

// The node at index in a widget's connection list, in its short form.
static int connection(int node, int index) {
    return command(node, GET_CONNECTIONS, index & ~3) >> (index % 4 * 8) & 0xFF;
}

// Looks for a converter through the widgets a node takes from, depth first.
static bool find_dac(int node, uint8_t *path, uint8_t *input, int depth, int *length) {
    path[depth] = node;
    input[depth] = 0;
    if (widget_type(node) == WIDGET_DAC) {
        *length = depth + 1;
        return true;
    }
    if (depth + 1 == MAX_PATH)
        return false;

    int count = parameter(node, PARAM_CONNECTIONS) & 0x7F;

    for (int i = 0; i < count && i < 16; i++) {
        int from = connection(node, i);

        if (from && find_dac(from, path, input, depth + 1, length)) {
            input[depth] = i;
            if (count > 1)
                command(node, SET_SELECT, i);
            return true;
        }
    }
    return false;
}

static bool find_paths(int group) {
    uint32_t nodes = parameter(group, PARAM_NODES);
    int first = nodes >> 16 & 0xFF, count = nodes & 0xFF;

    for (int node = first; node < first + count && path_count < MAX_PATHS; node++) {
        if (widget_type(node) != WIDGET_PIN || !(parameter(node, PARAM_PIN) & PIN_OUTPUT)
            || command(node, GET_CONFIG, 0) >> 30 == CONFIG_NOTHING)
            continue;
        if (find_dac(node, paths[path_count], inputs[path_count], 0, &path_lengths[path_count]))
            path_count++;
    }
    return path_count > 0;
}

// Every widget on the way gets power and is unmuted at 0 dB, the pins send out
// what comes to them, and the converters play the stream.
static void set_up_paths(void) {
    for (int p = 0; p < path_count; p++) {
        for (int i = 0; i < path_lengths[p]; i++) {
            int node = paths[p][i];
            uint32_t caps = parameter(node, PARAM_WIDGET), amp = parameter(node, PARAM_OUT_AMP);

            command(node, SET_POWER, 0);
            if (caps & WIDGET_OUT_AMP)
                command(node, SET_AMP, AMP_OUT | AMP_BOTH | (amp & 0x7F));
            if (caps & WIDGET_IN_AMP)
                command(node, SET_AMP, AMP_IN | AMP_BOTH | inputs[p][i] << 8 | (parameter(node, PARAM_IN_AMP) & 0x7F));
        }

        int pin = paths[p][0], dac = paths[p][path_lengths[p] - 1];
        bool headphone = (command(pin, GET_CONFIG, 0) >> 20 & 0xF) == DEVICE_HEADPHONE;

        command(pin, SET_PIN, PIN_OUT_ENABLE | (headphone ? PIN_HEADPHONE : 0));
        if (parameter(pin, PARAM_PIN) & PIN_EAPD)
            command(pin, SET_EAPD, 0x02);
        command(dac, SET_STREAM, STREAM_TAG << 4);
        command(dac, SET_FORMAT, format);
    }
}

static bool find_codec(void) {
    uint16_t present = r16(STATESTS);

    for (codec = 0; codec < 15; codec++) {
        if (!(present & 1 << codec))
            continue;

        uint32_t nodes = parameter(0, PARAM_NODES);
        int first = nodes >> 16 & 0xFF, count = nodes & 0xFF;

        for (int group = first; group < first + count; group++) {
            if ((parameter(group, PARAM_FUNCTION) & 0xFF) == FUNCTION_AUDIO) {
                command(group, SET_POWER, 0);
                if (find_paths(group))
                    return true;
            }
        }
    }
    return false;
}

static void *dma_page(uint64_t *phys) {
    *phys = mm_alloc_kernel_page();
    if (!*phys)
        return NULL;
    memset(mm_phys_to_virt(*phys), 0, PAGE_SIZE);
    return mm_phys_to_virt(*phys);
}

static bool start_rings(void) {
    uint64_t corb_phys, rirb_phys;

    corb = dma_page(&corb_phys);
    rirb = dma_page(&rirb_phys);
    if (!corb || !rirb)
        return false;

    w8(CORBCTL, 0);
    w8(RIRBCTL, 0);
    wait_for(CORBCTL, DMA_RUN, 0, 8);
    wait_for(RIRBCTL, DMA_RUN, 0, 8);
    w8(CORBSIZE, RING_SIZE_256);
    w8(RIRBSIZE, RING_SIZE_256);
    w32(CORBLBASE, corb_phys);
    w32(CORBUBASE, corb_phys >> 32);
    w32(RIRBLBASE, rirb_phys);
    w32(RIRBUBASE, rirb_phys >> 32);

    // Some controllers show the reset bit back before it can be cleared again.
    w16(CORBRP, RING_RESET);
    wait_for(CORBRP, RING_RESET, RING_RESET, 16);
    w16(CORBRP, 0);
    wait_for(CORBRP, RING_RESET, 0, 16);
    w16(CORBWP, 0);
    w16(RIRBWP, RING_RESET);
    w16(RINTCNT, 0xFF);
    rirb_read = 0;
    w8(CORBCTL, DMA_RUN);
    w8(RIRBCTL, DMA_RUN);
    return true;
}

static bool reset_controller(void) {
    w32(GCTL, r32(GCTL) & ~GCTL_RUNNING);
    if (!wait_for(GCTL, GCTL_RUNNING, 0, 32))
        return false;
    w32(GCTL, r32(GCTL) | GCTL_RUNNING);
    if (!wait_for(GCTL, GCTL_RUNNING, GCTL_RUNNING, 32))
        return false;
    // Codecs have 521 us after that to say they're there.
    for (int i = 0; i < TIMEOUT && !r16(STATESTS); i++)
        __asm__ volatile("pause");
    return r16(STATESTS) != 0;
}

static bool set_up_stream(void) {
    uint64_t bdl_phys;
    struct bdl_entry *bdl = dma_page(&bdl_phys);

    if (!bdl)
        return false;
    for (int i = 0; i < PAGES; i++) {
        uint64_t phys;

        pages[i] = dma_page(&phys);
        if (!pages[i])
            return false;
        bdl[i] = (struct bdl_entry){phys, PAGE_SIZE, 0};
    }
    if (!(r16(GCAP) >> 12 & 0xF))
        return false; // no output streams
    stream = STREAMS + (r16(GCAP) >> 8 & 0xF) * 0x20; // the first output stream
    w32(stream + SD_BDPL, bdl_phys);
    w32(stream + SD_BDPU, bdl_phys >> 32);
    return true;
}

// The format register's bits for a rate: 48 or 44.1 kHz, times and divided by a little.
static int rate_bits(int rate) {
    static const struct { int rate, bits; } rates[] = {
        {48000, 0x0000}, {44100, 0x4000}, {96000, 0x0800}, {88200, 0x4800}, {32000, 0x0A00}, {24000, 0x0100},
        {22050, 0x4100}, {16000, 0x0200}, {11025, 0x4300}, {8000, 0x0500}, {192000, 0x1800}, {176400, 0x5800},
    };

    for (size_t i = 0; i < sizeof(rates) / sizeof(*rates); i++) {
        if (rates[i].rate == rate)
            return rates[i].bits;
    }
    return -1;
}

bool hda_supports(int rate) {
    return rate_bits(rate) >= 0;
}

void hda_set_rate(int rate) {
    format = rate_bits(rate) | FORMAT_16_BIT | FORMAT_STEREO;
    for (int p = 0; p < path_count; p++)
        command(paths[p][path_lengths[p] - 1], SET_FORMAT, format);
}

void hda_stop(void) {
    w8(stream + SD_CTL, r8(stream + SD_CTL) & ~SD_RUN);
    wait_for(stream + SD_CTL, SD_RUN, 0, 8);
    for (int i = 0; i < PAGES; i++)
        memset(pages[i], 0, PAGE_SIZE);
}

// A stream starts from the beginning of the buffer after a reset.
void hda_start(void) {
    w8(stream + SD_CTL, SD_RESET);
    wait_for(stream + SD_CTL, SD_RESET, SD_RESET, 8);
    w8(stream + SD_CTL, 0);
    wait_for(stream + SD_CTL, SD_RESET, 0, 8);
    w32(stream + SD_CBL, HDA_BUFFER_BYTES);
    w16(stream + SD_LVI, PAGES - 1);
    w16(stream + SD_FMT, format);
    w8(stream + SD_CTL + 2, STREAM_TAG << 4);
    w8(stream + SD_STS, 0x1C); // clears what it's said before
    w8(stream + SD_CTL, SD_RUN);
}

uint32_t hda_played(void) {
    return r32(stream + SD_LPIB) % HDA_BUFFER_BYTES;
}

void hda_put(uint32_t offset, const void *data, uint32_t len) {
    const uint8_t *from = data;

    for (uint32_t done = 0; done < len;) {
        uint32_t at = (offset + done) % HDA_BUFFER_BYTES, part = PAGE_SIZE - at % PAGE_SIZE;

        part = part < len - done ? part : len - done;
        memcpy(pages[at / PAGE_SIZE] + at % PAGE_SIZE, from + done, part);
        done += part;
    }
}

static struct pci_device found;
static bool any;

static void look(struct pci_device device) {
    if (!any && pci_read(device, PCI_CLASS) >> 16 == 0x0403) { // multimedia, audio
        found = device;
        any = true;
    }
}

bool hda_init(void) {
    pci_for_each(look);
    if (!any)
        return false;

    pci_enable(found);

    uint32_t bar = pci_read(found, PCI_BAR0);
    uint64_t base = bar & ~0xFULL;

    if ((bar & 0x6) == 0x4)
        base |= (uint64_t)pci_read(found, PCI_BAR0 + 4) << 32;
    registers = mm_map_device(base, REGISTERS_SIZE);
    format = FORMAT_16_BIT | FORMAT_STEREO;
    if (!registers || !reset_controller() || !start_rings() || !find_codec() || !set_up_stream()) {
        kprintf("hda: found a controller, but nowhere to play\n");
        registers = NULL;
        return false;
    }
    w32(INTCTL, 0);
    set_up_paths();
    kprintf("hda: codec %u, playing through %u pin%s\n", codec, path_count, path_count == 1 ? "" : "s");
    return true;
}
