/*
 * Downloads go to ~/Downloads, under the name the server gives, or one after it
 * if that's taken, and a notification says when each is done, with a button to
 * show it in Files.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "utils/errors.h"
#include "netsurf/download.h"
#include "desktop/download.h"

/* libgui's, declared here as its gui.h has names NetSurf's headers use too. */
struct gui_window;
bool gui_notify(struct gui_window *window, const char *title, const char *text);
bool gui_notify_action(struct gui_window *window, const char *title, const char *text, const char *label,
		const char *command);

struct gui_download_window {
	FILE *file;
	char path[1024];
	bool failed;
};

static struct gui_download_window *download_create(struct download_context *ctx, struct gui_window *parent)
{
	const char *home = getenv("HOME"), *name = download_context_get_filename(ctx);
	struct gui_download_window *dw = calloc(1, sizeof(*dw));
	char dir[512];

	(void)parent;
	if (dw == NULL)
		return NULL;
	snprintf(dir, sizeof(dir), "%s/Downloads", home && strcmp(home, "/") ? home : "");
	mkdir(dir, 0755);
	if (name == NULL || name[0] == '\0' || strchr(name, '/'))
		name = "download";
	snprintf(dw->path, sizeof(dw->path), "%s/%s", dir, name);
	for (int i = 2; access(dw->path, F_OK) == 0; i++)
		snprintf(dw->path, sizeof(dw->path), "%s/%d-%s", dir, i, name);
	if ((dw->file = fopen(dw->path, "wb")) == NULL) {
		free(dw);
		return NULL;
	}
	return dw;
}

static nserror download_data(struct gui_download_window *dw, const char *data, unsigned int size)
{
	if (fwrite(data, 1, size, dw->file) != size) {
		dw->failed = true;
		return NSERROR_SAVE_FAILED;
	}
	return NSERROR_OK;
}

static void download_error(struct gui_download_window *dw, const char *error_msg)
{
	dw->failed = true;
	gui_notify(NULL, "Download failed", error_msg ? error_msg : strrchr(dw->path, '/') + 1);
}

static void download_done(struct gui_download_window *dw)
{
	char command[1100];

	if (fclose(dw->file) != 0)
		dw->failed = true;
	if (dw->failed) {
		unlink(dw->path);
	} else {
		snprintf(command, sizeof(command), "files --select '%s'", dw->path);
		gui_notify_action(NULL, "Downloaded", strrchr(dw->path, '/') + 1, "Show", command);
	}
	free(dw);
}

static struct gui_download_table download_table = {
	.create = download_create,
	.data = download_data,
	.error = download_error,
	.done = download_done,
};

struct gui_download_table *framebuffer_download_table = &download_table;
