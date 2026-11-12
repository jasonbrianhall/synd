/* make_mac_icon.m - the macOS app icon: res/syndicate-icon.png (a round badge) on a dark tile of the shape and size
 * of Apple's icon grid (a 1024 px canvas, an 824 px rounded square 100 px in from each edge, corners of 185 px).
 * Recent macOS shows an icon that does not fill that shape inside a grey plate; this one fills it.
 *
 *   clang -fobjc-arc -framework Cocoa tools/make_mac_icon.m -o /tmp/make_mac_icon
 *   /tmp/make_mac_icon res/syndicate-icon.png res/macos/syndicate-icon-macos.png
 *
 * The result is committed; the build makes the .icns from it (sips + iconutil). */
#import <Cocoa/Cocoa.h>

int main(int argc, char **argv)
{
    @autoreleasepool {
        const CGFloat size = 1024, inset = 100, side = 824, radius = 185.4, badge = 760;
        NSImage *art;
        NSBitmapImageRep *rep;
        NSBezierPath *tile;
        NSGradient *bg;
        NSShadow *sh = [[NSShadow alloc] init];
        if (argc != 3) {
            fprintf(stderr, "usage: make_mac_icon IN.png OUT.png\n");
            return 2;
        }
        art = [[NSImage alloc] initWithContentsOfFile:@(argv[1])];
        if (art == nil) return 1;
        rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:(NSInteger)size
            pixelsHigh:(NSInteger)size bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
            colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:0 bitsPerPixel:32];
        [NSGraphicsContext saveGraphicsState];
        NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        NSGraphicsContext.currentContext.imageInterpolation = NSImageInterpolationHigh;
        /* the tile: near-black with the game's green glowing from the middle */
        tile = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(inset, inset, side, side) xRadius:radius yRadius:radius];
        bg = [[NSGradient alloc] initWithStartingColor:[NSColor colorWithSRGBRed:0.10 green:0.22 blue:0.08 alpha:1]
                                           endingColor:[NSColor colorWithSRGBRed:0.02 green:0.03 blue:0.02 alpha:1]];
        [bg drawInBezierPath:tile relativeCenterPosition:NSZeroPoint];
        /* the badge, with a soft shadow, clipped to the tile */
        [tile addClip];
        sh.shadowColor = [NSColor colorWithWhite:0 alpha:0.6];
        sh.shadowBlurRadius = 18;
        sh.shadowOffset = NSMakeSize(0, -8);
        [sh set];
        [art drawInRect:NSMakeRect((size - badge) / 2, (size - badge) / 2, badge, badge) fromRect:NSZeroRect
              operation:NSCompositingOperationSourceOver fraction:1];
        [NSGraphicsContext restoreGraphicsState];
        return [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[2])
                                                                                        atomically:YES] ? 0 : 1;
    }
}
