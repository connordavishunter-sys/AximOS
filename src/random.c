#include <errno_values.h>
#include <irq.h>
#include <process.h>
#include <random.h>
#include <serial.h>
#include <string.h>

#define KEY_SIZE 32
#define CHACHA_BLOCK_SIZE 64
#define BLAKE2S_BLOCK_SIZE 64
#define BITS_TO_BE_READY 256
#define CPU_BITS_PER_WORD 64
#define CPU_RETRIES 32
#define RESEED_TICKS (60 * TIMER_HZ)

#define CPUID_RDRAND (1u << 30) // leaf 1, ecx
#define CPUID_RDSEED (1u << 18) // leaf 7, ebx

static uint32_t pool[KEY_SIZE / 4];
static uint32_t key[KEY_SIZE / 4];
static uint32_t generator_nonce;
static uint32_t bits_in_pool;
static bool has_rdrand, has_rdseed;
static bool ready;
static uint64_t last_reseed;

static const uint32_t blake2s_iv[8] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
                                       0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};

static const uint8_t blake2s_sigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
};

static uint32_t rotate_right(uint32_t value, int count) {
    return value >> count | value << (32 - count);
}

static uint32_t rotate_left(uint32_t value, int count) {
    return value << count | value >> (32 - count);
}

#define BLAKE2S_G(a, b, c, d, x, y)      \
    do {                                 \
        v[a] += v[b] + (x);              \
        v[d] = rotate_right(v[d] ^ v[a], 16); \
        v[c] += v[d];                    \
        v[b] = rotate_right(v[b] ^ v[c], 12); \
        v[a] += v[b] + (y);              \
        v[d] = rotate_right(v[d] ^ v[a], 8);  \
        v[c] += v[d];                    \
        v[b] = rotate_right(v[b] ^ v[c], 7);  \
    } while (0)

// Compresses one block into h. x86 being little-endian, it can be read as words.
static void blake2s_compress(uint32_t *h, const void *block, uint64_t counter, bool last) {
    uint32_t m[16], v[16];

    memcpy(m, block, sizeof(m));
    memcpy(v, h, 32);
    memcpy(v + 8, blake2s_iv, 32);
    v[12] ^= (uint32_t)counter;
    v[13] ^= (uint32_t)(counter >> 32);
    if (last)
        v[14] = ~v[14];

    for (int round = 0; round < 10; round++) {
        const uint8_t *s = blake2s_sigma[round];

        BLAKE2S_G(0, 4, 8, 12, m[s[0]], m[s[1]]);
        BLAKE2S_G(1, 5, 9, 13, m[s[2]], m[s[3]]);
        BLAKE2S_G(2, 6, 10, 14, m[s[4]], m[s[5]]);
        BLAKE2S_G(3, 7, 11, 15, m[s[6]], m[s[7]]);
        BLAKE2S_G(0, 5, 10, 15, m[s[8]], m[s[9]]);
        BLAKE2S_G(1, 6, 11, 12, m[s[10]], m[s[11]]);
        BLAKE2S_G(2, 7, 8, 13, m[s[12]], m[s[13]]);
        BLAKE2S_G(3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
    for (int i = 0; i < 8; i++)
        h[i] ^= v[i] ^ v[i + 8];
}

// A 32 byte BLAKE2s hash of a prefix of the same size followed by data.
static void hash_with_prefix(uint32_t *out, const uint32_t *prefix, const void *data, size_t len) {
    uint32_t h[8];
    uint8_t block[BLAKE2S_BLOCK_SIZE] = {0};
    const uint8_t *from = data;
    uint64_t total = KEY_SIZE + len;
    uint64_t done = 0;
    size_t used = KEY_SIZE;

    memcpy(h, blake2s_iv, sizeof(h));
    h[0] ^= 0x01010000 | KEY_SIZE;
    memcpy(block, prefix, KEY_SIZE);

    for (;;) {
        size_t part = BLAKE2S_BLOCK_SIZE - used < len ? BLAKE2S_BLOCK_SIZE - used : len;

        memcpy(block + used, from, part);
        used += part;
        from += part;
        len -= part;
        if (len == 0)
            break;

        done += BLAKE2S_BLOCK_SIZE;
        blake2s_compress(h, block, done, false);
        memset(block, 0, sizeof(block));
        used = 0;
    }
    blake2s_compress(h, block, total, true);
    memcpy(out, h, KEY_SIZE);
}

#define CHACHA_QUARTER(a, b, c, d)                      \
    do {                                                \
        x[a] += x[b];                                   \
        x[d] = rotate_left(x[d] ^ x[a], 16);            \
        x[c] += x[d];                                   \
        x[b] = rotate_left(x[b] ^ x[c], 12);            \
        x[a] += x[b];                                   \
        x[d] = rotate_left(x[d] ^ x[a], 8);             \
        x[c] += x[d];                                   \
        x[b] = rotate_left(x[b] ^ x[c], 7);             \
    } while (0)

static void chacha20_block(uint32_t *out, const uint32_t *chacha_key, uint32_t counter, uint32_t nonce0, uint32_t nonce1, uint32_t nonce2) {
    uint32_t state[16] = {0x61707865, 0x3320646E, 0x79622D32, 0x6B206574};
    uint32_t x[16];

    memcpy(state + 4, chacha_key, KEY_SIZE);
    state[12] = counter;
    state[13] = nonce0;
    state[14] = nonce1;
    state[15] = nonce2;
    memcpy(x, state, sizeof(x));

    for (int i = 0; i < 10; i++) {
        CHACHA_QUARTER(0, 4, 8, 12);
        CHACHA_QUARTER(1, 5, 9, 13);
        CHACHA_QUARTER(2, 6, 10, 14);
        CHACHA_QUARTER(3, 7, 11, 15);
        CHACHA_QUARTER(0, 5, 10, 15);
        CHACHA_QUARTER(1, 6, 11, 12);
        CHACHA_QUARTER(2, 7, 8, 13);
        CHACHA_QUARTER(3, 4, 9, 14);
    }
    for (int i = 0; i < 16; i++)
        out[i] = x[i] + state[i];
}

void random_add_data(const void *data, size_t len) {
    hash_with_prefix(pool, pool, data, len);
}

static void add_bits(uint32_t bits) {
    bits_in_pool += bits;
    if (!ready && bits_in_pool >= BITS_TO_BE_READY) {
        ready = true;
        last_reseed = 0;
        kprintf("random: ready\n");
        process_wakeup(&ready);
    }
}

// Sets the carry flag on success, which it can fail to do when asked too often.
static bool cpu_word(uint64_t *out, bool seed) {
    for (int i = 0; i < CPU_RETRIES; i++) {
        unsigned char ok;

        if (seed)
            __asm__ volatile("rdseed %0; setc %1" : "=r"(*out), "=qm"(ok) : : "cc");
        else
            __asm__ volatile("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok) : : "cc");
        if (ok)
            return true;
        __asm__ volatile("pause");
    }
    return false;
}

// RDSEED is the raw source. RDRAND is stretched from it, so only RDSEED gets counted
// fully. Either way it's hashed with the rest, so a bad one can only fail to help.
static void add_from_cpu(int words) {
    for (int i = 0; i < words; i++) {
        uint64_t value;

        if (has_rdseed && cpu_word(&value, true)) {
            random_add_data(&value, sizeof(value));
            add_bits(CPU_BITS_PER_WORD);
        } else if (has_rdrand && cpu_word(&value, false)) {
            random_add_data(&value, sizeof(value));
            add_bits(CPU_BITS_PER_WORD);
        }
    }
}

static void cpuid(uint32_t leaf, uint32_t *ebx, uint32_t *ecx) {
    uint32_t eax = leaf, edx;

    *ecx = 0;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(*ebx), "+c"(*ecx), "=d"(edx));
}

void random_init(void) {
    uint32_t ebx, ecx;
    uint32_t top_ebx, top_ecx;
    uint32_t top_leaf = 0;

    __asm__ volatile("cpuid" : "+a"(top_leaf), "=b"(top_ebx), "=c"(top_ecx) : : "edx");
    cpuid(1, &ebx, &ecx);
    has_rdrand = ecx & CPUID_RDRAND;
    if (top_leaf >= 7) {
        cpuid(7, &ebx, &ecx);
        has_rdseed = ebx & CPUID_RDSEED;
    }

    uint64_t start = __builtin_ia32_rdtsc();

    random_add_data(&start, sizeof(start));
    add_from_cpu(BITS_TO_BE_READY / CPU_BITS_PER_WORD);
    kprintf("random: %s\n", has_rdseed ? "using RDSEED" : has_rdrand ? "using RDRAND" : "no CPU source, using interrupt timing");
}

void random_add_interrupt(uint64_t vector) {
    struct {
        uint64_t time;
        uint64_t vector;
    } event = {__builtin_ia32_rdtsc(), vector};

    random_add_data(&event, sizeof(event));
    if (!ready)
        add_bits(1);
}

static void reseed(void) {
    last_reseed = irq_ticks();
    add_from_cpu(1);
    hash_with_prefix(key, key, pool, KEY_SIZE);
    hash_with_prefix(pool, pool, key, KEY_SIZE);
}

bool random_ready(void) {
    return ready;
}

// Each call uses up the key it started with, so what came out earlier can't be worked
// out from the state afterwards.
void random_bytes(void *out, size_t len) {
    uint8_t *to = out;
    uint32_t block[CHACHA_BLOCK_SIZE / 4];
    uint32_t counter = 1;

    if (last_reseed == 0 || irq_ticks() - last_reseed >= RESEED_TICKS)
        reseed();

    generator_nonce++;
    chacha20_block(block, key, 0, generator_nonce, 0, 0);
    memcpy(key, block, KEY_SIZE);

    while (len > 0) {
        size_t part = len < CHACHA_BLOCK_SIZE ? len : CHACHA_BLOCK_SIZE;

        chacha20_block(block, key, counter++, generator_nonce, 0, 0);
        memcpy(to, block, part);
        to += part;
        len -= part;
    }

    chacha20_block(block, key, counter, generator_nonce, 1, 0);
    memcpy(key, block, KEY_SIZE);
    memset(block, 0, sizeof(block));
}

int random_wait_ready(bool nonblocking) {
    while (!ready) {
        if (nonblocking)
            return -EAGAIN;
        if (!process_sleep(&ready))
            return -EINTR;
    }
    return 0;
}
