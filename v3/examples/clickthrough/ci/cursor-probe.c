/* CI probe: dump the current X11 cursor image via XFixes.
 *   cursor-probe [outfile.ppm]
 * Prints "serial=N WxH sum=S" — the serial identifies the cursor shape, so
 * comparing serials at two spots proves the shape actually changed.
 * Optionally writes the image as a flat PPM for human inspection. */
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) { fprintf(stderr, "cannot open display\n"); return 1; }
	int event_base, error_base;
	if (!XFixesQueryExtension(dpy, &event_base, &error_base)) {
		fprintf(stderr, "XFixes extension unavailable\n");
		return 1;
	}
	XFixesCursorImage *ci = XFixesGetCursorImage(dpy);
	if (!ci) { fprintf(stderr, "no cursor image\n"); return 1; }

	unsigned long long sum = 0;
	for (int i = 0; i < ci->width * ci->height; i++) sum += ci->pixels[i];
	printf("serial=%lu w=%d h=%d sum=%llu hotspot=%d,%d\n",
	       (unsigned long)ci->cursor_serial, ci->width, ci->height, sum,
	       ci->xhot, ci->yhot);

	if (argc > 1) {
		FILE *f = fopen(argv[1], "wb");
		if (f) {
			fprintf(f, "P3\n%d %d\n255\n", ci->width, ci->height);
			for (long i = 0; i < (long)ci->width * ci->height; i++) {
				unsigned long p = ci->pixels[i];
				int a = (int)((p >> 24) & 0xFF);
				int r = (int)((p >> 16) & 0xFF);
				int g = (int)((p >> 8) & 0xFF);
				int b = (int)(p & 0xFF);
				/* XFixes pixels are premultiplied ARGB */
				if (a > 0) { r = r * 255 / a; g = g * 255 / a; b = b * 255 / a; }
				fprintf(f, "%d %d %d\n", r, g, b);
			}
			fclose(f);
		}
	}
	XFree(ci);
	XCloseDisplay(dpy);
	return 0;
}
