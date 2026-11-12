/* macos_devtest.m - development checks of the macOS app (built only with -DSYN_DEVTEST=ON).
 *
 * The app is checked without touching the rest of the desktop: input is made into NSEvents and posted to this
 * process's own event queue ([NSApp postEvent:]), so it takes the same way through AppKit as real input (menu
 * shortcuts, the window, the view's handlers), and nothing reaches another program.  Snapshots read the app's own
 * windows, which needs no screen recording permission.
 *
 *   SYN_TEST_DIR=DIR SYN_TEST_SCRIPT="ms:action;ms:action;..." Syndicate64.app/Contents/MacOS/Syndicate64 ...
 *
 * ms is the time from the start; the actions:
 *   key:CODE[:MODS]    a key pressed and released 100 ms later (CODE a macOS virtual key code in hex; MODS letters c Command,
 *                      s Shift, o Option, t Control, held around the key with their own flagsChanged events)
 *   down:CODE / up:CODE   a key pressed or released only
 *   mods:MODS          the modifiers now held (flagsChanged events for each change; "mods:" releases all)
 *   move:X:Y / click:X:Y / rclick:X:Y   the pointer at X, Y (0..1 of the view) / a left / right click there
 *   delta:DX:DY        relative motion (points), as a captured pointer sees it
 *   snap:NAME          DIR/NAME_view.png (the view's layers) and DIR/NAME_window.png (the window as composited)
 *   about[:NAME]       opens the About box (snapshot DIR/NAME.png of it when NAME is given) ; aboutok closes it
 *   appearance:dark|light|system   the app's appearance
 *   close              the window's close button
 *   fatal              host_fatal() from another thread ; snapmodal:NAME  DIR/NAME.png of the open dialog
 *   mini / unmini      minimizes the window / brings it back
 *   full               the "Full Screen" menu item ; state  logs macos_state() ; quit  Cmd+Q
 * Every action is logged with host_log ("devtest: ..."), so a --log file shows what happened when. */
#import "macos_devtest.h"

#include <dlfcn.h>

#include "../../rt/host.h"

static NSWindow *g_win;
static NSView *g_v;
static NSString *g_dir, *g_about_name;
static NSWindow *g_about;

/* `b` on the main thread after `ms`, also while a modal dialog runs (GCD's main queue waits then) */
static void later(double ms, void (^b)(void))
{
    NSTimer *t = [NSTimer timerWithTimeInterval:ms / 1000.0 repeats:NO block:^(NSTimer *x) {
      (void)x;
      b();
    }];
    [NSRunLoop.mainRunLoop addTimer:t forMode:NSRunLoopCommonModes];
    [NSRunLoop.mainRunLoop addTimer:t forMode:NSModalPanelRunLoopMode];
}

static void dlog(NSString *s)
{
    host_log([@"devtest: " stringByAppendingString:s].UTF8String);
}

static void write_png(NSBitmapImageRep *rep, NSString *name)
{
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    [png writeToFile:[g_dir stringByAppendingPathComponent:name] atomically:YES];
}

/* the whole window as the window server composites it: an app may read its own windows without the screen
 * recording permission (CGWindowListCreateImage, looked up at run time as newer SDKs hide it) */
static void snap_window(NSWindow *w, NSString *name)
{
    typedef CGImageRef (*ListImageFn)(CGRect, uint32_t, uint32_t, uint32_t);
    ListImageFn fn = (ListImageFn)dlsym(RTLD_DEFAULT, "CGWindowListCreateImage");
    CGImageRef img;
    if (fn == NULL) return;
    img = fn(CGRectNull, 8u /* including this window */, (uint32_t)w.windowNumber, 1u /* no shadow */);
    if (img == NULL) return;
    write_png([[NSBitmapImageRep alloc] initWithCGImage:img], name);
    CGImageRelease(img);
}

/* the view's layers at its full pixel size */
static void snap_view(NSString *name)
{
    NSRect px = [g_v convertRectToBacking:g_v.bounds];
    int w = (int)px.size.width, h = (int)fabs(px.size.height);
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
        bytesPerRow:0 bitsPerPixel:32];
    CGContextRef cg = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep].CGContext;
    CGContextScaleCTM(cg, (CGFloat)w / g_v.bounds.size.width, (CGFloat)h / g_v.bounds.size.height);
    /* the layers of a flipped view render upside down into a bitmap context: turn the context over */
    CGContextTranslateCTM(cg, 0, g_v.bounds.size.height);
    CGContextScaleCTM(cg, 1, -1);
    [g_v.layer renderInContext:cg];
    write_png(rep, name);
}

/* ---- events, posted to this process only ---- */
static CGEventFlags g_flags; /* the modifiers the script holds */

static void post_cg(CGEventRef e)
{
    NSEvent *ne = [NSEvent eventWithCGEvent:e];
    CFRelease(e);
    if (ne != nil) [NSApp postEvent:ne atStart:NO];
}

/* modifier: mac key code, CGEventFlags bit (with the left-hand device bit) */
static const struct {
    char letter;
    uint16_t code;
    CGEventFlags flag;
} g_mods[] = {{'c', 0x37, kCGEventFlagMaskCommand | 0x08},
              {'s', 0x38, kCGEventFlagMaskShift | 0x02},
              {'o', 0x3A, kCGEventFlagMaskAlternate | 0x20},
              {'t', 0x3B, kCGEventFlagMaskControl | 0x01}};

static void set_mods(const char *m)
{
    size_t i;
    for (i = 0; i < sizeof g_mods / sizeof g_mods[0]; i++) {
        int want = strchr(m, g_mods[i].letter) != NULL, have = (g_flags & g_mods[i].flag) == g_mods[i].flag;
        CGEventRef e;
        if (want == have) continue;
        g_flags = want ? (g_flags | g_mods[i].flag) : (g_flags & ~g_mods[i].flag);
        e = CGEventCreate(NULL);
        CGEventSetType(e, kCGEventFlagsChanged);
        CGEventSetIntegerValueField(e, kCGKeyboardEventKeycode, g_mods[i].code);
        CGEventSetFlags(e, g_flags);
        post_cg(e);
    }
}

static void key(int code, int down)
{
    CGEventRef e = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)code, down ? true : false);
    CGEventSetFlags(e, g_flags);
    post_cg(e);
}

/* a point of the view (fractions 0..1) in the global coordinates CGEvents use (top-left of the main display) */
static CGPoint global_point(double fx, double fy)
{
    NSRect b = g_v.bounds;
    NSPoint p = NSMakePoint(b.size.width * fx, b.size.height * fy); /* the view is flipped: y down */
    NSPoint s = [g_win convertPointToScreen:[g_v convertPoint:p toView:nil]];
    return CGPointMake(s.x, NSMaxY(NSScreen.screens[0].frame) - s.y);
}

static void mouse(CGEventType t, CGPoint p, CGMouseButton b, int dx, int dy)
{
    CGEventRef e = CGEventCreateMouseEvent(NULL, t, p, b);
    CGEventSetFlags(e, g_flags);
    if (dx || dy) {
        CGEventSetIntegerValueField(e, kCGMouseEventDeltaX, dx);
        CGEventSetIntegerValueField(e, kCGMouseEventDeltaY, dy);
    }
    /* a click must name its window (a posted event has none); a move goes to the key window anyway */
    CGEventSetIntegerValueField(e, kCGMouseEventWindowUnderMousePointer, g_win.windowNumber);
    CGEventSetIntegerValueField(e, kCGMouseEventWindowUnderMousePointerThatCanHandleThisEvent, g_win.windowNumber);
    if (t != kCGEventMouseMoved) {
        NSEvent *ne = [NSEvent eventWithCGEvent:e];
        NSEventType nt = t == kCGEventLeftMouseDown    ? NSEventTypeLeftMouseDown
                         : t == kCGEventLeftMouseUp    ? NSEventTypeLeftMouseUp
                         : t == kCGEventRightMouseDown ? NSEventTypeRightMouseDown
                                                       : NSEventTypeRightMouseUp;
        NSPoint s = NSMakePoint(p.x, NSMaxY(NSScreen.screens[0].frame) - p.y);
        NSPoint w = [g_win convertPointFromScreen:s];
        NSEvent *me = [NSEvent mouseEventWithType:nt location:w modifierFlags:ne.modifierFlags
                                        timestamp:ne.timestamp windowNumber:g_win.windowNumber context:nil
                                      eventNumber:0 clickCount:1 pressure:1.0f];
        CFRelease(e);
        [NSApp postEvent:me atStart:NO];
        return;
    }
    post_cg(e);
}

static void run_action(NSString *act)
{
    NSArray<NSString *> *a = [act componentsSeparatedByString:@":"];
    NSString *k = a[0];
    char st[256];
    dlog(act);
    if ([k isEqualToString:@"key"] || [k isEqualToString:@"down"] || [k isEqualToString:@"up"]) {
        /* a key is held 100 ms, as by a hand (the game reads some keys by their state, not from the buffer) */
        int code = (int)strtol(a[1].UTF8String, NULL, 16);
        CGEventFlags before = g_flags;
        NSString *mods = a.count > 2 ? a[2] : nil;
        BOOL press = ![k isEqualToString:@"up"], release = ![k isEqualToString:@"down"];
        if (mods != nil) set_mods(mods.UTF8String);
        if (press) key(code, 1);
        later(press && release ? 100 : 0, ^{
          if (release) key(code, 0);
          if (mods != nil) {
              /* back to the modifiers held before */
              char m[8] = "";
              size_t i, n = 0;
              for (i = 0; i < sizeof g_mods / sizeof g_mods[0]; i++)
                  if ((before & g_mods[i].flag) == g_mods[i].flag) m[n++] = g_mods[i].letter;
              set_mods(m);
          }
        });
    } else if ([k isEqualToString:@"mods"]) {
        set_mods(a.count > 1 ? a[1].UTF8String : "");
    } else if ([k isEqualToString:@"move"] || [k isEqualToString:@"click"] || [k isEqualToString:@"rclick"]) {
        CGPoint p = global_point(a[1].doubleValue, a[2].doubleValue);
        int right = [k isEqualToString:@"rclick"];
        mouse(kCGEventMouseMoved, p, kCGMouseButtonLeft, 0, 0);
        if (![k isEqualToString:@"move"]) {
            mouse(right ? kCGEventRightMouseDown : kCGEventLeftMouseDown, p, right ? kCGMouseButtonRight : kCGMouseButtonLeft, 0, 0);
            later(150, ^{
              mouse(right ? kCGEventRightMouseUp : kCGEventLeftMouseUp, p, right ? kCGMouseButtonRight : kCGMouseButtonLeft, 0, 0);
            });
        }
    } else if ([k isEqualToString:@"delta"]) {
        mouse(kCGEventMouseMoved, global_point(0.5, 0.5), kCGMouseButtonLeft, a[1].intValue, a[2].intValue);
    } else if ([k isEqualToString:@"snap"]) {
        snap_view([a[1] stringByAppendingString:@"_view.png"]);
        snap_window(g_win, [a[1] stringByAppendingString:@"_window.png"]);
    } else if ([k isEqualToString:@"about"]) {
        g_about_name = a.count > 1 ? a[1] : nil;
        [NSApp sendAction:@selector(about:) to:nil from:nil]; /* modal: the script goes on inside it */
    } else if ([k isEqualToString:@"aboutok"]) {
        if (g_about != nil) [NSApp stopModal];
    } else if ([k isEqualToString:@"appearance"]) {
        NSApp.appearance = [a[1] isEqualToString:@"dark"]    ? [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]
                           : [a[1] isEqualToString:@"light"] ? [NSAppearance appearanceNamed:NSAppearanceNameAqua]
                                                             : nil;
    } else if ([k isEqualToString:@"fatal"]) {
        /* as the core reports an emulation error: from another thread, which then waits for good */
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
          host_fatal("A test of the error report (devtest).");
        });
    } else if ([k isEqualToString:@"snapmodal"]) {
        for (NSWindow *w in NSApp.windows) /* the dialog: a visible window other than the game's */
            if (w != g_win && w.isVisible && w.frame.size.height > 60) snap_window(w, [a[1] stringByAppendingString:@".png"]);
    } else if ([k isEqualToString:@"close"]) {
        [g_win performClose:nil];
    } else if ([k isEqualToString:@"mini"]) {
        [g_win miniaturize:nil];
    } else if ([k isEqualToString:@"unmini"]) {
        [g_win deminiaturize:nil];
    } else if ([k isEqualToString:@"full"]) {
        [NSApp sendAction:@selector(toggleFull:) to:nil from:nil];
    } else if ([k isEqualToString:@"state"]) {
        macos_state(st, sizeof st);
        dlog(@(st));
    } else if ([k isEqualToString:@"quit"]) {
        set_mods("c");
        key(0x0C, 1);
        key(0x0C, 0);
        set_mods("");
    }
}

void devtest_about(NSWindow *about)
{
    g_about = about;
    if (g_about_name == nil) return;
    {
        NSString *name = g_about_name;
        g_about_name = nil;
        later(600, ^{
          snap_window(about, [name stringByAppendingString:@".png"]);
        });
    }
}

void devtest_start(NSWindow *window, NSView *view)
{
    NSDictionary *env = NSProcessInfo.processInfo.environment;
    NSString *script = env[@"SYN_TEST_SCRIPT"];
    g_win = window;
    g_v = view;
    g_dir = env[@"SYN_TEST_DIR"] ?: NSTemporaryDirectory();
    [NSFileManager.defaultManager createDirectoryAtPath:g_dir withIntermediateDirectories:YES attributes:nil error:nil];
    if (script == nil) return;
    for (NSString *item in [script componentsSeparatedByString:@";"]) {
        NSRange c = [item rangeOfString:@":"];
        NSString *act;
        long long ms;
        if (c.location == NSNotFound) continue;
        ms = [item substringToIndex:c.location].longLongValue;
        act = [item substringFromIndex:c.location + 1];
        later((double)ms, ^{
          run_action(act);
        });
    }
}
