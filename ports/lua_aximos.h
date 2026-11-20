// Included before Lua's own sources: its prompt reads lines with AximOS's line
// editor, keeping their history in ~/.lua_history. Ctrl+C gives an empty line,
// rather than ending the prompt.
#include <errno.h>
#include <lineedit.h>
#include <stdio.h>

// Lua's POSIX build jumps without saving the signal mask, which setjmp here never does.
#define _setjmp setjmp
#define _longjmp longjmp

static inline char *lua_aximos_line(const char *prompt) {
    static char empty[1];
    char *line = line_read(prompt, NULL);

    if (line || errno != EINTR)
        return line;
    putchar('\n');
    return empty;
}

#define lua_initreadline(L) ((void)L, line_history(".lua_history"))
#define lua_readline(L, b, p) ((void)L, ((b) = lua_aximos_line(p)) != NULL)
#define lua_saveline(L, line) ((void)L, (void)line)
#define lua_freeline(L, b) ((void)L, (void)b)
