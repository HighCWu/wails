//go:build darwin

package application

/*
#cgo darwin LDFLAGS: -framework AppKit
#import <AppKit/AppKit.h>

// wailsMouseLocation returns the cursor position in the same top-left
// coordinate space as windowGetPosition: Cocoa screen points (bottom-left
// origin) converted with the primary screen height. Runs on the main thread.
// Reading NSEvent.mouseLocation requires no TCC permission.
static void wailsMouseLocation(int* x, int* y) {
	NSPoint mouse = [NSEvent mouseLocation];
	NSScreen* primaryScreen = [[NSScreen screens] firstObject];
	if (primaryScreen == NULL) {
		primaryScreen = [NSScreen mainScreen];
	}
	CGFloat primaryHeight = [primaryScreen frame].size.height;
	*x = (int)mouse.x;
	*y = (int)(primaryHeight - mouse.y);
}
*/
import "C"

func init() {
	MousePositionFunc = func() (int, int, bool) {
		var x, y C.int
		InvokeSync(func() {
			C.wailsMouseLocation(&x, &y)
		})
		return int(x), int(y), true
	}
}
