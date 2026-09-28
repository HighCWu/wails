//go:build linux && gtk3 && cgo

package main

/*
#cgo pkg-config: gtk+-3.0
#include <gtk/gtk.h>

static gboolean passive_realize(GSignalInvocationHint* hint, guint count,
                                const GValue* values, gpointer data) {
    GtkWidget* widget = g_value_get_object(values);
    if (GTK_IS_WINDOW(widget)) {
        gtk_window_set_accept_focus(GTK_WINDOW(widget), FALSE);
        gtk_window_set_focus_on_map(GTK_WINDOW(widget), FALSE);
    }
    return TRUE;
}
static void configure_passive_windows(void) {
    gpointer klass = g_type_class_ref(GTK_TYPE_WIDGET);
    guint signal = g_signal_lookup("realize", GTK_TYPE_WIDGET);
    g_signal_add_emission_hook(signal, 0, passive_realize, NULL, NULL);
    g_type_class_unref(klass);
}
*/
import "C"

// Passive rendering verification must never request keyboard focus.
func configurePassiveWindows() { C.configure_passive_windows() }
