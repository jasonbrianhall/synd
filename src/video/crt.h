/* crt.h - a CRT scanline filter for the game picture (portable, no OS calls; the front ends share it).
 *
 * The picture is scaled to the output size the way a CRT draws it: every source line is a horizontal beam
 * with a Gaussian vertical profile whose width follows its brightness (bright lines bloom and close the gap,
 * dark lines stay thin), the light adds up in linear space, edges along a line are slightly soft, and on large
 * outputs a faint aperture-grille mask shows.  The average brightness is kept close to the unfiltered one.
 *
 * Use: crt_prepare() once per frame (single thread, reads the source), then crt_render_rows() over disjoint
 * row ranges of the output, from any number of threads at once. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CrtFilter CrtFilter;

CrtFilter *crt_create(void);
void crt_destroy(CrtFilter *f);

/* `src`: sw x sh pixels 0x00RRGGBB (row stride sw), shown as a dw x dh picture.  Returns 0 when the output
 * is too small for scanlines (under two output rows per source line); then draw the picture unfiltered. */
int crt_prepare(CrtFilter *f, const uint32_t *src, int sw, int sh, int dw, int dh);

/* output rows y0 <= y < y1 of the prepared frame into `dst`, which points at row y0 (pixels 0x00RRGGBB,
 * `stride` in pixels) */
void crt_render_rows(const CrtFilter *f, uint32_t *dst, int stride, int y0, int y1);

#ifdef __cplusplus
}
#endif
