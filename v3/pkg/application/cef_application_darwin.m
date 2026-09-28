//go:build darwin && cgo && wails_cef && !ios && !server

#import <Cocoa/Cocoa.h>
#include <stdint.h>
#define OS_MAC 1
#include "../../internal/cef/include/cef_application_mac.h"
extern int wailsCEFEnabled(void);
extern bool shouldQuitApplication(void);
extern void cleanup(void);
int wails_cef_enabled(void) { return wailsCEFEnabled(); }

@interface WailsCEFApplication : NSApplication <CefAppProtocol> {
  BOOL handlingSendEvent;
}
@end
@implementation WailsCEFApplication
- (BOOL)isHandlingSendEvent {
  return handlingSendEvent;
}
- (void)setHandlingSendEvent:(BOOL)value {
  handlingSendEvent = value;
}
// Let App.Run return so its CEF shutdown runs after all browser closes.
- (void)terminate:(id)sender {
  // Chromium may install an application delegate when its own windows (for
  // example DevTools) are opened. Wails owns the quit policy and host cleanup;
  // do not route its termination through Chromium's delegate.
  if (!shouldQuitApplication())
    return;
  cleanup();
  [self stop:sender];
  [self postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                     location:NSZeroPoint
                                modifierFlags:0
                                    timestamp:0
                                 windowNumber:0
                                      context:nil
                                      subtype:0
                                        data1:0
                                        data2:0]
          atStart:NO];
}
- (void)sendEvent:(NSEvent *)event {
  BOOL previous = handlingSendEvent;
  handlingSendEvent = YES;
  @try {
    [super sendEvent:event];
  } @finally {
    handlingSendEvent = previous;
  }
}
@end
void wails_cef_prepare_app(void) { [WailsCEFApplication sharedApplication]; }
void wails_cef_pump_host(void) {
  @autoreleasepool {
    for (int i = 0; i < 32; i++) {
      NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                      untilDate:[NSDate distantPast]
                                         inMode:NSDefaultRunLoopMode
                                        dequeue:YES];
      if (!e)
        break;
      [NSApp sendEvent:e];
    }
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
  }
}
uintptr_t wails_cef_content_view(void *window) {
  return (uintptr_t)[(NSWindow *)window contentView];
}
void wails_cef_size_view(uintptr_t parent, uintptr_t child) {
  if (!child)
    return;
  NSView *view = (NSView *)child;
  NSView *container = (NSView *)parent;
  [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
  if (!NSEqualRects(view.frame, container.bounds))
    [view setFrame:container.bounds];
}
void wails_cef_close_window(void *window, uintptr_t child) {
  @autoreleasepool {
    // Wails retains its NSWindow after close. Remove CEF's child explicitly:
    // CefBrowserHostView.dealloc is what notifies CEF of native destruction.
    if (child)
      [(NSView *)child removeFromSuperview];
    [(NSWindow *)window close];
  }
}
