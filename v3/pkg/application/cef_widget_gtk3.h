#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>

unsigned int wails_cef_keyval(unsigned int keycode) {
  Display* d = gdk_x11_get_default_xdisplay();
  XkbStateRec state = {0};
  XkbGetState(d, XkbUseCoreKbd, &state);
  return XkbKeycodeToKeysym(d, keycode, state.group, 0);
}

// GTK's preferred/system visual may differ from the X11 default visual.
// Chromium creates its embedded child using the default visual; a mismatch
// causes BadMatch during window creation and leaves an empty GTK container.
// Match cefclient's UseDefaultX11VisualForGtk, before realizing the widget.
static void wails_cef_set_default_visual(GtkWidget* widget) {
  GdkScreen* screen = gtk_widget_get_screen(widget);
  Display* display = GDK_SCREEN_XDISPLAY(screen);
  Visual* visual = DefaultVisual(display, GDK_SCREEN_XNUMBER(screen));
  GdkVisual* gvisual = gdk_x11_screen_lookup_visual(screen, visual->visualid);
  if (gvisual != NULL) gtk_widget_set_visual(widget, gvisual);
}

static unsigned long wails_cef_widget_xid(GtkWidget* widget) {
  GdkWindow* win = gtk_widget_get_window(widget);
  if (win == NULL) return 0;
  // CEF uses a separate X connection. Publish the parent before creating
  // its child on that connection.
  gdk_display_sync(gtk_widget_get_display(widget));
  return (unsigned long)gdk_x11_window_get_xid(win);
}

static void wails_cef_resize_browser(unsigned long xwin, int w, int h) {
  if (xwin == 0) return;
  Display* d = gdk_x11_get_default_xdisplay();
  XResizeWindow(d, (Window)xwin, (unsigned)w, (unsigned)h);
  XFlush(d);
}

// wails_cef_query_pointer returns the current pointer position relative
// to the root window plus the button state mask, used to synthesize drag
// state for gtk_window_begin_move_drag when GTK never saw the button
// press (it happened inside CEF's native child window).
static int wails_cef_query_pointer(GtkWidget* widget, int* btn, int* root_x, int* root_y) {
  GdkWindow* win = gtk_widget_get_window(widget);
  if (win == NULL) return 0;
  Display* d = gdk_x11_get_default_xdisplay();
  Window root = XDefaultRootWindow(d);
  Window rret, cret;
  int wx, wy;
  unsigned int mask = 0;
  if (!XQueryPointer(d, root, &rret, &cret, root_x, root_y, &wx, &wy, &mask)) return 0;
  int button = 0;
  if (mask & Button1Mask) button = 1;
  else if (mask & Button2Mask) button = 2;
  else if (mask & Button3Mask) button = 3;
  *btn = button;
  return 1;
}

// Focus may arrive before CEF maps its child. Retry only while the GTK
// toplevel itself owns X focus; never steal focus from another native window.
static int wails_cef_toplevel_has_xfocus(GtkWidget* widget) {
  GdkWindow* top = gtk_widget_get_window(gtk_widget_get_toplevel(widget));
  if (!top) return 0;
  Window focused; int revert;
  XGetInputFocus(gdk_x11_get_default_xdisplay(), &focused, &revert);
  Window top_xid = gdk_x11_window_get_xid(top);
  if (focused == top_xid) return 1;
  // GDK usually focuses a hidden 1x1 InputOnly proxy below its toplevel.
  // It is not the browser, and CEF cannot receive keys through that proxy.
  if (focused == None || focused == PointerRoot) return 0;
  XWindowAttributes attr;
  Display* d = gdk_x11_get_default_xdisplay();
  if (!XGetWindowAttributes(d, focused, &attr) || attr.class != InputOnly) return 0;
  Window root, parent, *children = NULL; unsigned int count = 0;
  int ok = XQueryTree(d, focused, &root, &parent, &children, &count);
  if (children) XFree(children);
  return ok && parent == top_xid;
}

// CEF's outer X window contains a DesktopWindowTreeHost input child.
// Focus that mapped child, matching CefWindowX11::Focus, rather than the
// outer wrapper (which does not receive Chromium keyboard events).
static void wails_cef_focus_browser(unsigned long host) {
  if (!host) return;
  Display* d = gdk_x11_get_default_xdisplay();
  Window root, parent, *children = NULL; unsigned int count = 0;
  if (XQueryTree(d, host, &root, &parent, &children, &count)) {
    for (unsigned int i = 0; i < count; ++i) {
      XWindowAttributes a;
      if (XGetWindowAttributes(d, children[i], &a) && a.map_state == IsViewable && a.width > 1) {
        XSetInputFocus(d, children[i], RevertToParent, CurrentTime);
        XFlush(d);
        break;
      }
    }
  }
  if (children) XFree(children);
}

extern void onUriList(char** files, gint x, gint y, gpointer data);
extern void onDragEnter(gpointer data);
extern void onDragOver(gint x, gint y, gpointer data);
extern void onDragLeave(gpointer data);

static void wails_cef_drop_received(GtkWidget* widget, GdkDragContext* context,
 gint x, gint y, GtkSelectionData* selection, guint info, guint time, gpointer data) {
 gchar** uris = gtk_selection_data_get_uris(selection);
 GPtrArray* paths = g_ptr_array_new_with_free_func(g_free);
 if (uris) {
  for (int i = 0; uris[i]; i++) {
   char* path = g_filename_from_uri(uris[i], NULL, NULL);
   if (path) g_ptr_array_add(paths, path);
  }
  g_strfreev(uris);
 }
 gboolean accepted = paths->len > 0;
 if (accepted) {
  g_ptr_array_add(paths, NULL);
  onUriList((char**)paths->pdata, x, y, data);
 }
 g_ptr_array_free(paths, TRUE);
 // GTK_DEST_DEFAULT_DROP completes the native drag after this signal.
 onDragLeave(data);
}
static gboolean wails_cef_drop_motion(GtkWidget* widget, GdkDragContext* context,
 gint x, gint y, guint time, gpointer data) {
 onDragEnter(data);
 onDragOver(x, y, data);
 return FALSE;
}
static void wails_cef_drop_leave(GtkWidget* widget, GdkDragContext* context,
 guint time, gpointer data) { onDragLeave(data); }
static void wails_cef_enable_dnd(GtkWidget* widget, uintptr_t id) {
 GtkTargetEntry target = {"text/uri-list", 0, 0};
 gtk_drag_dest_set(widget, GTK_DEST_DEFAULT_ALL, &target, 1, GDK_ACTION_COPY);
 g_signal_connect(widget, "drag-data-received", G_CALLBACK(wails_cef_drop_received), (gpointer)id);
 g_signal_connect(widget, "drag-motion", G_CALLBACK(wails_cef_drop_motion), (gpointer)id);
 g_signal_connect(widget, "drag-leave", G_CALLBACK(wails_cef_drop_leave), (gpointer)id);
}

// wails_cef_widget_size returns the widget's current allocation.
static void wails_cef_widget_size(GtkWidget* widget, int* w, int* h) {
  GtkAllocation a;
  gtk_widget_get_allocation(widget, &a);
  *w = a.width;
  *h = a.height;
}

// wails_cef_connect_map runs fn (an //export'ed Go callback) when the
// widget is first mapped, i.e. its GdkWindow exists.
extern void wailsCEFOnMap(GtkWidget* widget, gpointer user_data);
static void wails_cef_connect_map(GtkWidget* widget, gpointer user_data) {
  g_signal_connect(widget, "map", G_CALLBACK(wailsCEFOnMap), user_data);
}

// wails_cef_connect_focus forwards toplevel focus-in to the Go callback,
// which moves the X focus onto the browser window.
extern gboolean wailsCEFOnFocusIn(GtkWidget* widget, GdkEvent* event, gpointer user_data);
static void wails_cef_connect_focus(GtkWidget* widget) {
  g_signal_connect(widget, "focus-in-event", G_CALLBACK(wailsCEFOnFocusIn), NULL);
}

// wails_cef_connect_size_allocate tracks allocation changes so the
// embedded browser window can be resized to match.
extern void wailsCEFOnSizeAllocate(GtkWidget* widget, GdkRectangle* allocation, gpointer user_data);
static void wails_cef_connect_size_allocate(GtkWidget* widget, gpointer user_data) {
  g_signal_connect(widget, "size-allocate", G_CALLBACK(wailsCEFOnSizeAllocate), user_data);
}
