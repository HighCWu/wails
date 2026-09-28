// GTK4 widgets have no GdkWindow. Host CEF in a DefaultVisual X child.
#include <gtk/gtk.h>
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <stdint.h>
static Display* cef_display(GtkWidget* w) { GdkDisplay* d=gtk_widget_get_display(w); return GDK_IS_X11_DISPLAY(d)?gdk_x11_display_get_xdisplay(d):NULL; }
static Window cef_surface(GtkWidget* w) { GtkNative* n=gtk_widget_get_native(w); if(!n)return 0; GdkSurface* s=gtk_native_get_surface(n); return s&&GDK_IS_X11_SURFACE(s)?gdk_x11_surface_get_xid(s):0; }
unsigned int wails_cef_keyval(unsigned int keycode) {
 GdkDisplay* gd=gdk_display_get_default(); if(!GDK_IS_X11_DISPLAY(gd))return 0;
 Display* d=gdk_x11_display_get_xdisplay(gd); XkbStateRec state={0}; XkbGetState(d,XkbUseCoreKbd,&state); return XkbKeycodeToKeysym(d,keycode,state.group,0);
}
typedef struct {Display* display; Window window; Colormap colormap;} CEFChild;
static void cef_child_free(gpointer p) { CEFChild* c=p; XDestroyWindow(c->display,c->window); XFreeColormap(c->display,c->colormap); XFlush(c->display); g_free(c); }
static unsigned long wails_cef_widget_xid(GtkWidget* w) {
 Display* d=cef_display(w); Window parent=cef_surface(w); if(!d||!parent||!gtk_widget_get_mapped(w))return 0;
 CEFChild* c=g_object_get_data(G_OBJECT(w),"wails-cef-child"); int scale=gtk_widget_get_scale_factor(w);
 graphene_point_t zero={0,0},pos={0,0}; if(!gtk_widget_compute_point(w,GTK_WIDGET(gtk_widget_get_native(w)),&zero,&pos))return 0;
 int width=MAX(1,gtk_widget_get_width(w)*scale),height=MAX(1,gtk_widget_get_height(w)*scale);
 if(!c) {
  c=g_new0(CEFChild,1); c->display=d; int screen=DefaultScreen(d); Visual* visual=DefaultVisual(d,screen);
  c->colormap=XCreateColormap(d,parent,visual,AllocNone); XSetWindowAttributes a={0}; a.colormap=c->colormap; a.border_pixel=0; a.background_pixel=0;
  c->window=XCreateWindow(d,parent,pos.x*scale,pos.y*scale,width,height,0,DefaultDepth(d,screen),InputOutput,visual,CWColormap|CWBorderPixel|CWBackPixel,&a);
  g_object_set_data_full(G_OBJECT(w),"wails-cef-child",c,cef_child_free); XMapWindow(d,c->window);
 }
 XMoveResizeWindow(d,c->window,pos.x*scale,pos.y*scale,width,height); XSync(d,False); return c->window;
}
static void wails_cef_widget_size(GtkWidget* w,int* width,int* height) { wails_cef_widget_xid(w); int scale=gtk_widget_get_scale_factor(w); *width=gtk_widget_get_width(w)*scale; *height=gtk_widget_get_height(w)*scale; }
static void wails_cef_resize_browser(unsigned long win,int w,int h) { if(!win)return; Display* d=gdk_x11_display_get_xdisplay(gdk_display_get_default()); XResizeWindow(d,win,w,h); XFlush(d); }
static int wails_cef_query_pointer(GtkWidget* w,int* btn,int* x,int* y) { Display* d=cef_display(w); if(!d)return 0; Window root,child; int wx,wy; unsigned int mask; if(!XQueryPointer(d,DefaultRootWindow(d),&root,&child,x,y,&wx,&wy,&mask))return 0; *btn=(mask&Button1Mask)?1:(mask&Button2Mask)?2:(mask&Button3Mask)?3:0; return 1; }
static int wails_cef_toplevel_has_xfocus(GtkWidget* w) { Display* d=cef_display(w); Window top=cef_surface(w),focus; int revert; if(!d||!top)return 0; XGetInputFocus(d,&focus,&revert); return focus==top; }
static void wails_cef_focus_browser(unsigned long host) {
 if(!host)return; Display* d=gdk_x11_display_get_xdisplay(gdk_display_get_default()); Window root,parent,*children=NULL; unsigned int n=0;
 if(XQueryTree(d,host,&root,&parent,&children,&n)) { for(unsigned int i=0;i<n;i++) { XWindowAttributes a; if(XGetWindowAttributes(d,children[i],&a)&&a.map_state==IsViewable&&a.width>1){XSetInputFocus(d,children[i],RevertToParent,CurrentTime);break;} } }
 if(children)XFree(children); XFlush(d);
}
// Allocation and native focus are polled by the shared CEF message pump.
static void wails_cef_set_default_visual(GtkWidget* w) {}
static void wails_cef_connect_map(GtkWidget* w,gpointer p) {}
static void wails_cef_connect_size_allocate(GtkWidget* w,gpointer p) {}
static void wails_cef_connect_focus(GtkWidget* w) {}
static void wails_cef_enable_dnd(GtkWidget* w,uintptr_t id) {} // host installs GTK4 drop controller
#define gtk_widget_set_can_focus gtk_widget_set_focusable
static void gtk_widget_destroy(GtkWidget* w) { gtk_window_destroy(GTK_WINDOW(w)); }
static void gtk_window_get_size(GtkWindow* w,int* width,int* height) { *width=gtk_widget_get_width(GTK_WIDGET(w)); *height=gtk_widget_get_height(GTK_WIDGET(w)); }
static void gtk_window_get_position(GtkWindow* w,int* x,int* y) { Display* d=cef_display(GTK_WIDGET(w)); Window win=cef_surface(GTK_WIDGET(w)),child; *x=*y=0; if(d&&win)XTranslateCoordinates(d,win,DefaultRootWindow(d),0,0,x,y,&child); }
static void gtk_window_move(GtkWindow* w,int x,int y) { Display* d=cef_display(GTK_WIDGET(w)); Window win=cef_surface(GTK_WIDGET(w)); if(d&&win){XMoveWindow(d,win,x,y);XFlush(d);} }
static void gtk_window_resize(GtkWindow* w,int width,int height) { gtk_window_set_default_size(w,width,height); }
