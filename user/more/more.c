#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define BUFFER_SIZE 4096
#define TAB_WIDTH 8

static char input[BUFFER_SIZE];
static size_t input_len;
static size_t input_pos;
static int input_fd;

static char output[BUFFER_SIZE];
static size_t output_len;

static int rows = 25;
static int cols = 80;
static int line_count;
static int column;

// Returns the next byte of the file, or -1 at the end.
static int next_byte(void) {
    if (input_pos == input_len) {
        ssize_t count = read(input_fd, input, BUFFER_SIZE);

        if (count <= 0)
            return -1;
        input_len = count;
        input_pos = 0;
    }
    return (unsigned char)input[input_pos++];
}

static void flush_output(void) {
    write(STDOUT_FILENO, output, output_len);
    output_len = 0;
}

// Asks how far to go, returning false to quit. Space shows a page and enter one
// more line, which leaves the screen as full as it was.
static bool prompt_user(uint64_t offset, uint32_t size) {
    flush_output();

    // One key at a time, without it showing.
    struct termios settings, raw;
    bool on_terminal = tcgetattr(STDERR_FILENO, &settings) == 0;

    if (on_terminal) {
        raw = settings;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDERR_FILENO, TCSANOW, &raw);
    }

    if (size)
        printf("--More--(%d%%)", (int)(offset * 100 / size));
    else
        printf("--More--");

    for (;;) {
        char key;

        // Standard input may be the text being paged, so keys come from stderr,
        // which is still the terminal.
        if (read(STDERR_FILENO, &key, 1) != 1)
            key = 'q';

        if (key == 'q' || key == ' ' || key == '\n') {
            if (on_terminal)
                tcsetattr(STDERR_FILENO, TCSANOW, &settings);
            printf("\r\x1b[K");
            line_count = key == '\n' ? rows - 2 : 0;
            return key != 'q';
        }
    }
}

// Pages one file. Returns false if the user quit.
static bool page_file(uint32_t size) {
    uint64_t offset = 0;
    int c;

    input_len = input_pos = 0;
    while ((c = next_byte()) >= 0) {
        offset++;

        if (output_len == BUFFER_SIZE)
            flush_output();
        output[output_len++] = c;

        if (c == '\t')
            column += TAB_WIDTH - column % TAB_WIDTH;
        else if (c != '\n' && c != '\r')
            column++;

        if (c != '\n' && column < cols)
            continue;

        column = 0;
        if (++line_count < rows - 1)
            continue;

        // Don't ask to see more if the file has just ended.
        if (next_byte() < 0)
            break;
        input_pos--;

        if (!prompt_user(offset, size))
            return false;
    }
    flush_output();
    return true;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: more [file...]\n"
                            "Shows files or standard input one screen at a time.\n"
                            "  Space  next screen\n"
                            "  Enter  next line\n"
                            "  q      quit\n");

    termsize(&rows, &cols);

    if (argc < 2) {
        input_fd = STDIN_FILENO;
        page_file(0);
        return 0;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;

        if (stat(argv[i], &st) < 0 || S_ISDIR(st.st_mode)) {
            dprintf(STDERR_FILENO, "more: %s: %s\n", argv[i], S_ISDIR(st.st_mode) ? "Is a directory" : strerror(errno));
            status = 1;
            continue;
        }

        input_fd = open(argv[i], O_RDONLY);
        if (input_fd < 0) {
            dprintf(STDERR_FILENO, "more: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }

        if (argc > 2) {
            printf("::::::::::::::\n%s\n::::::::::::::\n", argv[i]);
            line_count += 3;
        }

        bool keep_going = page_file(st.st_size);
        close(input_fd);
        if (!keep_going)
            break;
    }
    return status;
}
