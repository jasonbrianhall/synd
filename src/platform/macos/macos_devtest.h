/* macos_devtest.h - development checks of the macOS app (built only with -DSYN_DEVTEST=ON, never in a release):
 * scripted input posted inside the app's own process and snapshots of what its windows show.  See
 * macos_devtest.m. */
#pragma once

#import <Cocoa/Cocoa.h>

/* after the window is up: starts the script of SYN_TEST_SCRIPT, if set */
void devtest_start(NSWindow *window, NSView *view);
/* the About box is about to run (modal): snapshots it when the script asked for that */
void devtest_about(NSWindow *about);
/* implemented by macos_main.m: one line on the front end's state, for the script's log */
void macos_state(char *out, size_t n);
