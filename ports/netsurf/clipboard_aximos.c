/*
 * NetSurf's clipboard is AximOS's, which wm keeps, so text copies to and from
 * other programs.
 */
#include <stdbool.h>
#include <stddef.h>

#include "utils/errors.h"
#include "netsurf/clipboard.h"

#include "framebuffer/clipboard.h"

/* libgui's, declared here as its gui.h has names NetSurf's headers use too. */
bool gui_clipboard_set(const char *text, size_t length);
char *gui_clipboard_get(size_t *length);

/* The text, from malloc, which the core then owns. */
static void get_clipboard(char **buffer, size_t *length)
{
	*buffer = gui_clipboard_get(length);
	if (*buffer == NULL)
		*length = 0;
}

static void set_clipboard(const char *buffer, size_t length, nsclipboard_styles styles[], int n_styles)
{
	(void)styles, (void)n_styles;
	gui_clipboard_set(buffer, length);
}

static struct gui_clipboard_table clipboard_table = {
	.get = get_clipboard,
	.set = set_clipboard,
};

struct gui_clipboard_table *framebuffer_clipboard_table = &clipboard_table;
