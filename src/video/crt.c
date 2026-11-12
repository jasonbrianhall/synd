/* crt.c - the CRT scanline filter (see crt.h).
 *
 * Linear light is kept in 12 bits (0..4095).  crt_prepare() scales every source line to the output width
 * (sharpened linear interpolation: edges about two output pixels wide) into `lines`; crt_render_rows() then
 * mixes, for each output row, the two nearest source lines with Gaussian beam weights from a table indexed by
 * the distance to the line centre and the value of the colour channel itself, applies the mask and encodes
 * the result back to sRGB. */
#include "crt.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LIN_MAX 4095
#define DIST_STEPS 64   /* distance to a line centre, in 1/64 of the line spacing */
#define LEVELS 64       /* channel value buckets for the beam width */
#define ONE 1024        /* fixed-point 1.0 of the weights */

/* the look */
#define GAMMA 2.2
#define BEAM_MIN 0.20   /* beam sigma of a black line, in line spacings */
#define BEAM_MAX 0.32   /* of a full-intensity line */
#define BRIGHT_KEEP 0.8 /* how much of the darkening by the gaps is made up (0 none, 1 all) */
#define MASK_DIM 0.16   /* how much the aperture grille dims the two other channels of a column */

struct CrtFilter {
    uint16_t dec[256];               /* sRGB byte -> linear */
    uint8_t enc[LIN_MAX + 1];        /* linear -> sRGB byte */
    uint16_t beam[DIST_STEPS + 1][LEVELS];
    uint16_t mask[3][3];             /* [column % 3][channel], 1.0 = ONE, brightness made up */
    int sw, sh, dw, dh, use_mask;
    int *xi;                         /* per output column: left source pixel */
    uint16_t *xw;                    /* weight of the right source pixel, 0..256 */
    uint16_t *lines;                 /* sh lines of dw pixels, 4 values each (r, g, b, unused) */
    size_t lines_cap;
    int xcap;
};

static double gauss(double d, double s)
{
    return exp(-0.5 * (d / s) * (d / s));
}

CrtFilter *crt_create(void)
{
    CrtFilter *f = (CrtFilter *)calloc(1, sizeof *f);
    int i, d, l;
    if (f == NULL) return NULL;
    for (i = 0; i < 256; i++) f->dec[i] = (uint16_t)(pow(i / 255.0, GAMMA) * LIN_MAX + 0.5);
    for (i = 0; i <= LIN_MAX; i++) f->enc[i] = (uint8_t)(pow(i / (double)LIN_MAX, 1.0 / GAMMA) * 255.0 + 0.5);
    for (l = 0; l < LEVELS; l++) {
        double v = (l + 0.5) / LEVELS;
        double s = BEAM_MIN + (BEAM_MAX - BEAM_MIN) * sqrt(v);
        /* the mean light of a flat field of this level, over one line spacing, from the beams around it */
        double mean = 0, norm;
        int k, n = 256;
        for (k = 0; k < n; k++) {
            double y = (k + 0.5) / n, sum = 0;
            int j;
            for (j = -2; j <= 3; j++) sum += gauss(y - j, s);
            mean += sum;
        }
        mean /= n;
        norm = pow(1.0 / mean, BRIGHT_KEEP);
        for (d = 0; d <= DIST_STEPS; d++) {
            double w = gauss((double)d / DIST_STEPS, s) * norm * ONE + 0.5;
            f->beam[d][l] = (uint16_t)(w > 65535 ? 65535 : w);
        }
    }
    for (i = 0; i < 3; i++)
        for (d = 0; d < 3; d++) {
            double gain = 1.0 / (1.0 - MASK_DIM * 2.0 / 3.0);
            f->mask[i][d] = (uint16_t)((i == d ? 1.0 : 1.0 - MASK_DIM) * gain * ONE + 0.5);
        }
    return f;
}

void crt_destroy(CrtFilter *f)
{
    if (f == NULL) return;
    free(f->xi);
    free(f->xw);
    free(f->lines);
    free(f);
}

static int setup(CrtFilter *f, int sw, int sh, int dw, int dh)
{
    size_t need = (size_t)sh * (size_t)dw * 4u;
    int x;
    if (dw > f->xcap) {
        int *xi = (int *)realloc(f->xi, (size_t)dw * sizeof *xi);
        uint16_t *xw;
        if (xi == NULL) return 0;
        f->xi = xi;
        xw = (uint16_t *)realloc(f->xw, (size_t)dw * sizeof *xw);
        if (xw == NULL) return 0;
        f->xw = xw;
        f->xcap = dw;
    }
    if (need > f->lines_cap) {
        uint16_t *l = (uint16_t *)realloc(f->lines, need * sizeof *l);
        if (l == NULL) return 0;
        f->lines = l;
        f->lines_cap = need;
    }
    if (sw != f->sw || dw != f->dw) {
        double scale = (double)dw / sw, sharp = scale * 0.5 > 1.0 ? scale * 0.5 : 1.0;
        for (x = 0; x < dw; x++) {
            double u = (x + 0.5) / scale - 0.5, t;
            int i = (int)floor(u);
            t = (u - i - 0.5) * sharp + 0.5;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            if (i < 0) {
                i = 0;
                t = 0;
            }
            if (i >= sw - 1) {
                i = sw - 1;
                t = 0;
            }
            f->xi[x] = i;
            f->xw[x] = (uint16_t)(t * 256 + 0.5);
        }
    }
    f->sw = sw;
    f->sh = sh;
    f->dw = dw;
    f->dh = dh;
    f->use_mask = dw >= sw * 3; /* the grille needs at least three output columns per source pixel */
    return 1;
}

int crt_prepare(CrtFilter *f, const uint32_t *src, int sw, int sh, int dw, int dh)
{
    int y, x;
    if (f == NULL || sw <= 0 || sh <= 0 || dw <= 0 || dh < sh * 2) return 0;
    if (!setup(f, sw, sh, dw, dh)) return 0;
    for (y = 0; y < sh; y++) {
        const uint32_t *s = src + (size_t)y * sw;
        uint16_t *o = f->lines + (size_t)y * dw * 4u;
        for (x = 0; x < dw; x++, o += 4) {
            uint32_t a = s[f->xi[x]], b = s[f->xi[x] + (f->xi[x] < sw - 1)];
            uint32_t w = f->xw[x], v = 256u - w;
            o[0] = (uint16_t)((f->dec[(a >> 16) & 255] * v + f->dec[(b >> 16) & 255] * w) >> 8);
            o[1] = (uint16_t)((f->dec[(a >> 8) & 255] * v + f->dec[(b >> 8) & 255] * w) >> 8);
            o[2] = (uint16_t)((f->dec[a & 255] * v + f->dec[b & 255] * w) >> 8);
        }
    }
    return 1;
}

void crt_render_rows(const CrtFilter *f, uint32_t *dst, int stride, int y0, int y1)
{
    static const uint16_t no_mask[3] = {ONE, ONE, ONE};
    int y, x, c;
    for (y = y0; y < y1; y++) {
        /* source line centres sit at integer positions */
        double pos = (y + 0.5) * f->sh / f->dh - 0.5;
        int r0 = (int)floor(pos);
        int dq0 = (int)((pos - r0) * DIST_STEPS + 0.5), dq1 = DIST_STEPS - dq0;
        const uint16_t *l0 = r0 >= 0 ? f->lines + (size_t)r0 * f->dw * 4u : NULL;
        const uint16_t *l1 = r0 + 1 < f->sh ? f->lines + (size_t)(r0 + 1) * f->dw * 4u : NULL;
        const uint16_t *w0 = f->beam[dq0], *w1 = f->beam[dq1];
        uint32_t *o = dst + (size_t)(y - y0) * stride;
        for (x = 0; x < f->dw; x++) {
            const uint16_t *m = f->use_mask ? f->mask[x % 3] : no_mask;
            uint32_t px = 0;
            for (c = 0; c < 3; c++) {
                uint32_t v = 0;
                if (l0 != NULL) {
                    uint32_t a = l0[x * 4 + c];
                    v += a * w0[a >> 6];
                }
                if (l1 != NULL) {
                    uint32_t b = l1[x * 4 + c];
                    v += b * w1[b >> 6];
                }
                v = (uint32_t)(((uint64_t)v * m[c]) >> 20);
                if (v > LIN_MAX) v = LIN_MAX;
                px |= (uint32_t)f->enc[v] << (16 - 8 * c);
            }
            o[x] = px;
        }
    }
}
