#pragma once

// libgui on Linux has both backends, wm's and SDL's, so each is compiled with its
// functions named for it, as BACKEND_gui_..., and gui_switch.c picks between them.

#define BACKEND_NAME(name) BACKEND_PASTE(BACKEND, name)
#define BACKEND_PASTE(backend, name) BACKEND_PASTE_(backend, name)
#define BACKEND_PASTE_(backend, name) backend##_##name

#define gui_connect BACKEND_NAME(gui_connect)
#define gui_fd BACKEND_NAME(gui_fd)
#define gui_send BACKEND_NAME(gui_send)
#define gui_receive BACKEND_NAME(gui_receive)
#define gui_create_window BACKEND_NAME(gui_create_window)
#define gui_resize BACKEND_NAME(gui_resize)
#define gui_destroy_window BACKEND_NAME(gui_destroy_window)
#define gui_update BACKEND_NAME(gui_update)
#define gui_show BACKEND_NAME(gui_show)
#define gui_set_title BACKEND_NAME(gui_set_title)
#define gui_set_icon BACKEND_NAME(gui_set_icon)
#define gui_set_cursor BACKEND_NAME(gui_set_cursor)
#define gui_set_translucent BACKEND_NAME(gui_set_translucent)
#define gui_notify BACKEND_NAME(gui_notify)
#define gui_notify_action BACKEND_NAME(gui_notify_action)
#define gui_clipboard_set BACKEND_NAME(gui_clipboard_set)
#define gui_clipboard_get BACKEND_NAME(gui_clipboard_get)
#define gui_clipboard_set_image BACKEND_NAME(gui_clipboard_set_image)
#define gui_clipboard_get_image BACKEND_NAME(gui_clipboard_get_image)
#define gui_drag_file BACKEND_NAME(gui_drag_file)
#define gui_dropped_file BACKEND_NAME(gui_dropped_file)
#define gui_backend_wait BACKEND_NAME(gui_backend_wait)
