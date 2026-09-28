//go:build linux && !gtk3 && cgo

package main

/*
#cgo pkg-config: gtk4
#include <gtk/gtk.h>
static gboolean passive_realize(GSignalInvocationHint* hint, guint count, const GValue* values, gpointer data) {
 GtkWidget* widget = g_value_get_object(values);
 if (GTK_IS_WINDOW(widget)) {
  GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(widget));
  if (GDK_IS_TOPLEVEL(surface)) gdk_toplevel_set_focusable(GDK_TOPLEVEL(surface), FALSE);
 }
 return TRUE;
}
static void configure_passive_windows(void) {
 gpointer klass = g_type_class_ref(GTK_TYPE_WIDGET);
 g_signal_add_emission_hook(g_signal_lookup("realize", GTK_TYPE_WIDGET), 0, passive_realize, NULL, NULL);
 g_type_class_unref(klass);
}
*/
import "C"

func configurePassiveWindows() { C.configure_passive_windows() }
