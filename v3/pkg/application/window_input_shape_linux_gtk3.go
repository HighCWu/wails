//go:build linux && cgo && gtk3 && !android && !server

package application

/*
#include <gtk/gtk.h>
#include <cairo/cairo.h>

// wailsSetInputShape toggles the X11 input shape of the toplevel and the
// webview GDK windows. enable=1 applies a 1x1 input region (tao's proven
// recipe: an entirely empty region is unreliable across GDK/X servers, a
// single corner pixel is behaviourally identical to full passthrough), so
// the whole window hierarchy stops receiving pointer input and events fall
// through to the windows stacked below, including windows of other
// applications. enable=0 unsets the shape entirely, so no stale region is
// left on the X server when the flag flips or the window resizes.
static void wailsSetInputShape(GtkWidget* toplevel, GtkWidget* webview, int enable) {
	GdkWindow* windows[2];
	windows[0] = gtk_widget_get_window(toplevel);
	windows[1] = gtk_widget_get_window(webview);
	for (int i = 0; i < 2; i++) {
		GdkWindow* window = windows[i];
		if (window == NULL) {
			continue;
		}
		if (enable) {
			cairo_rectangle_int_t corner = {0, 0, 1, 1};
			cairo_region_t* region = cairo_region_create_rectangle(&corner);
			gdk_window_input_shape_combine_region(window, region, 0, 0);
			cairo_region_destroy(region);
		} else {
			gdk_window_input_shape_combine_region(window, NULL, 0, 0);
		}
	}
}
*/
import "C"

import "unsafe"

// setInputShape enables or disables pointer input for this window's whole
// window hierarchy. True passthrough requires shaping both the toplevel and
// the webview GDK windows: the webview is a separate X11 window, so masking
// the toplevel alone leaves it consuming clicks.
func (w *linuxWebviewWindow) setInputShape(ignore bool) {
	enable := C.int(0)
	if ignore {
		enable = 1
	}
	C.wailsSetInputShape(
		(*C.GtkWidget)(unsafe.Pointer(w.window)),
		(*C.GtkWidget)(unsafe.Pointer(w.webview)),
		enable,
	)
}
