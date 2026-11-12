/* audio.c - the sound hardware the games program: the Intel 8237 DMA controller, a Sound Blaster DSP 2.01
 * (8-bit mono DMA playback, as the AIL/32 "a32sbdg" driver uses it) and a Yamaha YM3812 (OPL2, AdLib) for
 * the "a32adlib" music driver.  Everything is driven by elapsed time: audio_poll() produces the samples due
 * since the last call, consumes DMA bytes at the DSP rate and raises the Sound Blaster interrupt when a block
 * ends.  The result goes to the platform as signed 16-bit stereo at host_audio_rate(). */
#include <math.h>
#include <string.h>

#include "host.h"
#include "rt.h"

#define SB_BASE 0x220u
#define SB_IRQ 7
#define SB_DMA 1
#define OPL_RATE 49716.0 /* 14.31818 MHz / 4 / 72 */

/* ==== 8237 DMA controller (channels 0-3) ================================================================= */
typedef struct {
    uint16_t base_addr, base_count, cur_addr, cur_count;
    uint8_t page, mode, masked, tc;
} DmaChan;
static DmaChan g_dma[4];
static int g_dma_ff; /* byte flip-flop */
static const uint16_t k_page_port[4] = {0x87, 0x83, 0x81, 0x82};

static void dma_out(uint16_t port, uint8_t v)
{
    int ch;
    if (port < 8) {
        DmaChan *c = &g_dma[port >> 1];
        if (port & 1) {
            c->base_count = g_dma_ff ? (uint16_t)((c->base_count & 0x00FF) | (v << 8)) : (uint16_t)((c->base_count & 0xFF00) | v);
            c->cur_count = c->base_count;
        } else {
            c->base_addr = g_dma_ff ? (uint16_t)((c->base_addr & 0x00FF) | (v << 8)) : (uint16_t)((c->base_addr & 0xFF00) | v);
            c->cur_addr = c->base_addr;
        }
        g_dma_ff ^= 1;
        return;
    }
    switch (port) {
    case 0x0A: g_dma[v & 3].masked = (v & 4) ? 1 : 0; return;
    case 0x0B: g_dma[v & 3].mode = v; return;
    case 0x0C: g_dma_ff = 0; return;
    case 0x0D:
        g_dma_ff = 0;
        for (ch = 0; ch < 4; ch++) g_dma[ch].masked = 1;
        return;
    case 0x0E:
        for (ch = 0; ch < 4; ch++) g_dma[ch].masked = 0;
        return;
    case 0x0F:
        for (ch = 0; ch < 4; ch++) g_dma[ch].masked = (uint8_t)((v >> ch) & 1);
        return;
    default:
        return;
    }
}

static uint8_t dma_in(uint16_t port)
{
    if (port < 8) {
        DmaChan *c = &g_dma[port >> 1];
        uint16_t v = (port & 1) ? c->cur_count : c->cur_addr;
        uint8_t r = g_dma_ff ? (uint8_t)(v >> 8) : (uint8_t)v;
        g_dma_ff ^= 1;
        return r;
    }
    if (port == 0x08) {
        uint8_t s = 0;
        int ch;
        for (ch = 0; ch < 4; ch++) {
            if (g_dma[ch].tc) s |= (uint8_t)(1 << ch);
            g_dma[ch].tc = 0;
        }
        return s;
    }
    return 0xFF;
}

/* one byte from a channel to the device; 0 when the channel is masked or finished (single cycle) */
static int dma_read_byte(int ch, uint8_t *out)
{
    DmaChan *c = &g_dma[ch];
    uint32_t addr;
    if (c->masked) return 0;
    addr = ((uint32_t)c->page << 16) | c->cur_addr;
    *out = (M != NULL && addr < RT_MEM_SIZE) ? M[addr] : 0x80;
    if (c->mode & 0x20)
        c->cur_addr--;
    else
        c->cur_addr++;
    if (c->cur_count-- == 0) {
        c->tc = 1;
        if (c->mode & 0x10) { /* auto-initialise */
            c->cur_addr = c->base_addr;
            c->cur_count = c->base_count;
        } else {
            c->masked = 1;
        }
    }
    return 1;
}

/* ==== Sound Blaster DSP 2.01 ============================================================================ */
static struct {
    uint8_t reset_latch;
    uint8_t out[16];
    int out_n;
    uint8_t cmd, args[4];
    int nargs, need;
    uint8_t test_reg, tc, speaker;
    uint32_t block;       /* auto-init block length (bytes - 1) */
    int playing, autoinit, paused;
    uint32_t left;        /* bytes left in the current block */
    double rate, phase;   /* samples per second; position between samples */
    int16_t cur, prev;    /* current and previous sample (for interpolation) */
    int irq_req;
    uint8_t dac;          /* direct DAC value */
} g_sb;

static void sb_push(uint8_t v)
{
    if (g_sb.out_n < (int)sizeof g_sb.out) g_sb.out[g_sb.out_n++] = v;
}

static void sb_reset(void)
{
    g_sb.out_n = 0;
    g_sb.nargs = g_sb.need = 0;
    g_sb.cmd = 0;
    g_sb.playing = g_sb.paused = g_sb.autoinit = 0;
    g_sb.speaker = 0;
    g_sb.cur = g_sb.prev = 0;
    sb_push(0xAA);
}

static void sb_start_dma(uint32_t len, int autoinit)
{
    g_sb.rate = 1000000.0 / (256 - g_sb.tc);
    g_sb.left = len + 1u;
    g_sb.autoinit = autoinit;
    g_sb.playing = 1;
    g_sb.paused = 0;
}

static int sb_args_for(uint8_t cmd)
{
    switch (cmd) {
    case 0x10: case 0x40: case 0xE0: case 0xE4: case 0x38:
        return 1;
    case 0x14: case 0x16: case 0x17: case 0x24: case 0x48: case 0x80: case 0x74: case 0x75: case 0x76: case 0x77:
        return 2;
    default:
        return 0;
    }
}

static void sb_command(void)
{
    uint8_t *a = g_sb.args;
    switch (g_sb.cmd) {
    case 0x10: g_sb.dac = a[0]; g_sb.cur = (int16_t)((a[0] - 128) << 8); break;
    case 0x14: case 0x16: case 0x17: case 0x74: case 0x75: case 0x76: case 0x77:
        sb_start_dma((uint32_t)(a[0] | (a[1] << 8)), 0);
        break;
    case 0x1C: case 0x1F: case 0x7D: case 0x7F: sb_start_dma(g_sb.block, 1); break;
    case 0x90: sb_start_dma(g_sb.block, 1); break; /* high speed, auto-init */
    case 0x91: sb_start_dma(g_sb.block, 0); break; /* high speed, single cycle */
    case 0x20: sb_push(0x80); break;               /* direct ADC: silence */
    case 0x24: g_sb.irq_req = 1; break;            /* ADC by DMA: nothing recorded */
    case 0x40: g_sb.tc = a[0]; break;
    case 0x48: g_sb.block = (uint32_t)(a[0] | (a[1] << 8)); break;
    case 0x80: g_sb.irq_req = 1; break;            /* silence block: finishes at once */
    case 0xD0: g_sb.paused = 1; break;
    case 0xD1: g_sb.speaker = 1; break;
    case 0xD3: g_sb.speaker = 0; break;
    case 0xD4: g_sb.paused = 0; break;
    case 0xD8: sb_push(g_sb.speaker ? 0xFF : 0x00); break;
    case 0xDA: g_sb.autoinit = 0; break;
    case 0xE0: sb_push((uint8_t)~a[0]); break;
    case 0xE1: sb_push(0x02); sb_push(0x01); break;
    case 0xE4: g_sb.test_reg = a[0]; break;
    case 0xE8: sb_push(g_sb.test_reg); break;
    case 0xF2: g_sb.irq_req = 1; break;
    case 0xF8: sb_push(0x00); break;
    default:
        rt_trace("sb: command %02x", g_sb.cmd);
        break;
    }
}

static void sb_write(uint8_t v)
{
    if (g_sb.need == 0) {
        g_sb.cmd = v;
        g_sb.need = sb_args_for(v);
        g_sb.nargs = 0;
        if (g_sb.need == 0) sb_command();
        return;
    }
    g_sb.args[g_sb.nargs++] = v;
    if (g_sb.nargs == g_sb.need) {
        g_sb.need = 0;
        sb_command();
    }
}

/* the next sample of DMA playback (or the held DAC value) */
static int16_t sb_fetch(void)
{
    uint8_t b;
    if (!g_sb.playing || g_sb.paused) return g_sb.cur;
    if (!dma_read_byte(SB_DMA, &b)) return g_sb.cur;
    if (--g_sb.left == 0) {
        g_sb.irq_req = 1;
        if (rt_verbose >= 3) rt_log("sb: block done, irq");
        if (g_sb.autoinit)
            g_sb.left = g_sb.block + 1u;
        else
            g_sb.playing = 0;
    }
    return (int16_t)((b - 128) << 8);
}

/* ==== YM3812 (OPL2) ==================================================================================== */
typedef struct {
    /* registers */
    uint8_t am, vib, egt, ksr, mult, ksl, tl, ar, dr, sl, rr, ws;
    /* state */
    uint32_t phase;
    int32_t env;  /* attenuation 0..511 (0.1875 dB) */
    int stage;    /* 0 attack 1 decay 2 sustain 3 release */
    int32_t out, prev;
    uint32_t eg_frac;
    int key;
} OplOp;

typedef struct {
    uint16_t fnum;
    uint8_t block, key, fb, con;
} OplCh;

static struct {
    uint8_t reg[256];
    OplOp op[18];
    OplCh ch[9];
    uint8_t wse, nts, dam, dvb, rhythm;
    uint8_t addr;
    uint8_t t1, t2, tctl, status;
    uint64_t t1_start, t2_start;
    uint64_t io_us;        /* ~1 us per port access, as on the ISA bus (timer tests poll the status port) */
    uint32_t sample;       /* sample counter (LFOs, envelope timing) */
    uint32_t noise;
    double pos;            /* output resampling position */
    int16_t last, cur;
} g_opl;

static int16_t g_logsin[256], g_exp[256];

/* operator index of slot register offset (0x00..0x15) */
static const int8_t k_slot_op[32] = {0, 1, 2, 3, 4, 5, -1, -1, 6, 7, 8, 9, 10, 11, -1, -1,
                                     12, 13, 14, 15, 16, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
/* channel of each operator, and whether it is the carrier */
static const uint8_t k_op_ch[18] = {0, 1, 2, 0, 1, 2, 3, 4, 5, 3, 4, 5, 6, 7, 8, 6, 7, 8};
static const uint8_t k_ch_mod[9] = {0, 1, 2, 6, 7, 8, 12, 13, 14};
static const uint8_t k_mult[16] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30};
static const uint8_t k_ksl_rom[16] = {0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64};
static const uint8_t k_ksl_shift[4] = {8, 1, 2, 0};

static void opl_tables(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        double s = sin((i + 0.5) * 3.14159265358979323846 / 512.0);
        g_logsin[i] = (int16_t)floor(-log(s) / log(2.0) * 256.0 + 0.5);
        g_exp[i] = (int16_t)floor((pow(2.0, (255 - i) / 256.0) - 1.0) * 1024.0 + 0.5);
    }
}

/* sine-family waveform at a 10-bit phase, attenuated by env (0.1875 dB units, scaled x8 as in the chip):
 * the log-sin / exp method of the real chip, result up to about +-4084 */
static int32_t opl_wave(int ws, uint32_t phase, int32_t att)
{
    uint32_t p = phase & 1023u;
    int neg = 0;
    int32_t ls, level, v;
    switch (ws) {
    case 0: neg = p >= 512; break;
    case 1: if (p >= 512) return 0; break;
    case 2: break;
    case 3: if (p & 256) return 0; break;
    }
    p &= 511u;
    ls = (p & 256) ? g_logsin[255 - (p & 255)] : g_logsin[p & 255];
    level = ls + (att << 3);
    if (level >= 0x1FFF) return 0;
    v = ((g_exp[level & 255] + 1024) << 1) >> (level >> 8);
    return neg ? -v - 1 : v;
}

static void opl_write(uint8_t r, uint8_t v)
{
    int slot, n;
    g_opl.reg[r] = v;
    if (r == 0x01) {
        g_opl.wse = (v >> 5) & 1;
        return;
    }
    if (r == 0x02) { g_opl.t1 = v; return; }
    if (r == 0x03) { g_opl.t2 = v; return; }
    if (r == 0x04) {
        if (v & 0x80) {
            g_opl.status = 0;
            return;
        }
        if ((v & 1) && !(g_opl.tctl & 1)) g_opl.t1_start = host_time_us() + g_opl.io_us;
        if ((v & 2) && !(g_opl.tctl & 2)) g_opl.t2_start = host_time_us() + g_opl.io_us;
        g_opl.tctl = v;
        return;
    }
    if (r == 0x08) {
        g_opl.nts = (v >> 6) & 1;
        return;
    }
    if (r >= 0x20 && r <= 0xF5 && (r & 0xE0) != 0xA0 && (r & 0xF0) != 0xB0 && (r & 0xF0) != 0xC0) {
        slot = k_slot_op[r & 0x1F];
        if (slot < 0) return;
        {
            OplOp *o = &g_opl.op[slot];
            switch (r & 0xE0) {
            case 0x20:
                o->am = (v >> 7) & 1; o->vib = (v >> 6) & 1; o->egt = (v >> 5) & 1; o->ksr = (v >> 4) & 1;
                o->mult = v & 15;
                break;
            case 0x40: o->ksl = (v >> 6) & 3; o->tl = v & 63; break;
            case 0x60: o->ar = (v >> 4) & 15; o->dr = v & 15; break;
            case 0x80: o->sl = (v >> 4) & 15; o->rr = v & 15; break;
            case 0xE0: o->ws = v & 3; break;
            }
        }
        return;
    }
    if (r >= 0xA0 && r <= 0xA8) {
        g_opl.ch[r - 0xA0].fnum = (uint16_t)((g_opl.ch[r - 0xA0].fnum & 0x300) | v);
        return;
    }
    if (r >= 0xB0 && r <= 0xB8) {
        OplCh *c = &g_opl.ch[r - 0xB0];
        int key = (v >> 5) & 1;
        c->fnum = (uint16_t)((c->fnum & 0xFF) | ((v & 3) << 8));
        c->block = (v >> 2) & 7;
        if (key != c->key) {
            int m = k_ch_mod[r - 0xB0];
            for (n = 0; n < 2; n++) {
                OplOp *o = &g_opl.op[m + n * 3];
                if (key) {
                    o->stage = 0;
                    o->phase = 0;
                } else {
                    o->stage = 3;
                }
            }
            c->key = (uint8_t)key;
        }
        return;
    }
    if (r == 0xBD) {
        uint8_t old = g_opl.rhythm;
        g_opl.dam = (v >> 7) & 1;
        g_opl.dvb = (v >> 6) & 1;
        g_opl.rhythm = v & 0x3F;
        if (v & 0x20) {
            /* drum key-ons: BD (ch 6 both ops), SD (op 16), TT (op 14), CY (op 17), HH (op 13) */
            static const struct { uint8_t bit; int8_t op[2]; } drums[5] = {
                {0x10, {12, 15}}, {0x08, {16, -1}}, {0x04, {14, -1}}, {0x02, {17, -1}}, {0x01, {13, -1}}};
            for (n = 0; n < 5; n++) {
                int on = (v & drums[n].bit) != 0, was = (old & 0x20) && (old & drums[n].bit);
                int k;
                if (on == was) continue;
                for (k = 0; k < 2; k++) {
                    OplOp *o;
                    if (drums[n].op[k] < 0) continue;
                    o = &g_opl.op[drums[n].op[k]];
                    if (on) {
                        o->stage = 0;
                        o->phase = 0;
                    } else {
                        o->stage = 3;
                    }
                }
            }
        }
        return;
    }
    if (r >= 0xC0 && r <= 0xC8) {
        g_opl.ch[r - 0xC0].fb = (v >> 1) & 7;
        g_opl.ch[r - 0xC0].con = v & 1;
        return;
    }
}

static uint8_t opl_status(void)
{
    uint64_t now = host_time_us() + g_opl.io_us;
    if ((g_opl.tctl & 1) && !(g_opl.tctl & 0x40) && now - g_opl.t1_start >= (uint64_t)(256 - g_opl.t1) * 80u)
        g_opl.status |= 0xC0;
    if ((g_opl.tctl & 2) && !(g_opl.tctl & 0x20) && now - g_opl.t2_start >= (uint64_t)(256 - g_opl.t2) * 320u)
        g_opl.status |= 0xA0;
    return (uint8_t)(g_opl.status | 0x06);
}

/* envelope: rate 0..63 -> how many 1-unit steps this sample (fractional, accumulated in eg_frac/65536) */
static int opl_eg_steps(OplOp *o, int rate)
{
    uint32_t inc;
    if (rate <= 0) return 0;
    if (rate > 63) rate = 63;
    /* one step every 2^(13 - rate/4) samples at x(4 + rate%4)/4 */
    {
        int sh = 13 - (rate >> 2);
        uint32_t frac = (uint32_t)(4 + (rate & 3));
        if (sh >= 0)
            inc = (frac << 14) >> sh; /* (frac/4) * 65536 / 2^sh */
        else
            inc = (frac << 14) << (-sh);
    }
    o->eg_frac += inc;
    {
        int steps = (int)(o->eg_frac >> 16);
        o->eg_frac &= 0xFFFFu;
        return steps;
    }
}

static void opl_envelope(OplOp *o, OplCh *c)
{
    int ks = (c->block << 1) | ((c->fnum >> (g_opl.nts ? 8 : 9)) & 1);
    int rof = o->ksr ? ks : ks >> 2;
    int steps;
    switch (o->stage) {
    case 0: /* attack */
        if (o->ar == 15) {
            o->env = 0;
        } else if (o->ar) {
            steps = opl_eg_steps(o, o->ar * 4 + rof);
            while (steps-- > 0 && o->env > 0) o->env -= (o->env >> 3) + 1;
        }
        if (o->env <= 0) {
            o->env = 0;
            o->stage = 1;
        }
        break;
    case 1: /* decay to the sustain level */
        if (o->dr) o->env += opl_eg_steps(o, o->dr * 4 + rof);
        {
            int32_t sl = o->sl == 15 ? 496 : o->sl * 16;
            if (o->env >= sl) {
                o->env = sl;
                o->stage = 2;
            }
        }
        break;
    case 2: /* sustain (EGT) or keep decaying at the release rate */
        if (!o->egt && o->rr) o->env += opl_eg_steps(o, o->rr * 4 + rof);
        break;
    case 3: /* release */
        if (o->rr) o->env += opl_eg_steps(o, o->rr * 4 + rof);
        break;
    }
    if (o->env > 511) o->env = 511;
}

static int32_t opl_op(OplOp *o, OplCh *c, int32_t mod, uint32_t trem, int vibrato_pos, int phase_only)
{
    uint32_t fnum = c->fnum, inc;
    int32_t att, ksl;
    if (o->vib) {
        int range = (int)(fnum >> 7) & 7;
        static const int8_t vib_pat[8] = {0, 1, 2, 1, 0, -1, -2, -1};
        int d = vib_pat[vibrato_pos];
        if (!g_opl.dvb) range >>= 1;
        fnum = (uint32_t)((int32_t)fnum + (d * range) / 2);
    }
    inc = ((fnum << c->block) * k_mult[o->mult]) >> 1;
    o->phase += inc;
    if (phase_only) return 0;
    opl_envelope(o, c);
    ksl = (k_ksl_rom[c->fnum >> 6] << 2) - ((8 - c->block) << 5);
    if (ksl < 0) ksl = 0;
    att = o->env + (o->tl << 2) + (ksl >> k_ksl_shift[o->ksl]) + (o->am ? (int32_t)trem : 0);
    if (att > 511) att = 511;
    return opl_wave(g_opl.wse ? o->ws : 0, (o->phase >> 10) + (uint32_t)mod, att);
}

static int32_t opl_sample(void)
{
    int n;
    int32_t mix = 0;
    uint32_t trem;
    int vib;
    uint32_t s = g_opl.sample++;
    /* tremolo: a triangle 0..105 at about 3.7 Hz, 1 dB or 4.8 dB deep */
    {
        uint32_t t = (s >> 6) % 210u;
        trem = t < 105 ? t : 210u - t;
        trem = g_opl.dam ? trem >> 2 : trem >> 4;
    }
    vib = (int)((s >> 10) & 7);
    g_opl.noise = (g_opl.noise >> 1) | ((((g_opl.noise) ^ (g_opl.noise >> 14)) & 1) << 22);
    if (g_opl.noise == 0) g_opl.noise = 1;

    for (n = 0; n < 9; n++) {
        OplCh *c = &g_opl.ch[n];
        OplOp *m = &g_opl.op[k_ch_mod[n]], *k = &g_opl.op[k_ch_mod[n] + 3];
        int32_t fbmod, mo, co;
        if ((g_opl.rhythm & 0x20) && n >= 7) break; /* 7 and 8 are drums */
        fbmod = c->fb ? (m->prev + m->out) >> (9 - c->fb) : 0;
        mo = opl_op(m, c, fbmod, trem, vib, 0);
        m->prev = m->out;
        m->out = mo;
        if ((g_opl.rhythm & 0x20) && n == 6) {
            /* bass drum: a normal 2-op channel at double output */
            co = opl_op(k, c, c->con ? 0 : mo, trem, vib, 0);
            mix += co * 2;
            continue;
        }
        co = opl_op(k, c, c->con ? 0 : mo, trem, vib, 0);
        k->out = co;
        mix += c->con ? mo + co : co;
    }
    if (g_opl.rhythm & 0x20) {
        OplCh *c7 = &g_opl.ch[7], *c8 = &g_opl.ch[8];
        OplOp *hh = &g_opl.op[13], *sd = &g_opl.op[16], *tt = &g_opl.op[14], *cy = &g_opl.op[17];
        uint32_t p13, p17, bit;
        int32_t att;
        /* tom-tom: plain sine on op 14 */
        mix += opl_op(tt, c8, 0, trem, vib, 0) * 2;
        /* advance hi-hat and cymbal phases, then build the metallic phase from their bits */
        opl_op(hh, c7, 0, trem, vib, 1);
        opl_op(cy, c8, 0, trem, vib, 1);
        p13 = hh->phase >> 10;
        p17 = cy->phase >> 10;
        bit = (((p13 >> 2) ^ (p13 >> 7)) | (p13 >> 3)) & 1;
        bit |= (((p17 >> 3) ^ (p17 >> 5)) & 1);
        /* hi-hat */
        opl_envelope(hh, c7);
        att = hh->env + (hh->tl << 2);
        if (att > 511) att = 511;
        mix += opl_wave(0, bit ? (0x200u | ((g_opl.noise & 1) ? 0xD0u : 0x34u)) : ((g_opl.noise & 1) ? 0x34u : 0xD0u),
                        att) * 2;
        /* snare: phase from bit 8 of the hi-hat phase and noise */
        opl_op(sd, c7, 0, trem, vib, 1);
        opl_envelope(sd, c7);
        att = sd->env + (sd->tl << 2);
        if (att > 511) att = 511;
        {
            uint32_t b8 = (p13 >> 8) & 1;
            uint32_t ph = (b8 << 9) | ((b8 ^ (g_opl.noise & 1)) << 8);
            mix += opl_wave(0, ph, att) * 2;
        }
        /* cymbal */
        opl_envelope(cy, c8);
        att = cy->env + (cy->tl << 2);
        if (att > 511) att = 511;
        mix += opl_wave(0, bit ? 0x300u : 0x100u, att) * 2;
    }
    return mix;
}

/* ==== ports ============================================================================================== */
void audio_port_out(uint16_t port, uint8_t v)
{
    if (rt_verbose >= 3) rt_log("audio out %03x <- %02x", port, v);
    if (port <= 0x0F) {
        dma_out(port, v);
        return;
    }
    if (port >= 0x80 && port <= 0x8F) {
        int ch;
        for (ch = 0; ch < 4; ch++)
            if (k_page_port[ch] == port) g_dma[ch].page = v;
        return;
    }
    if (port == 0x388 || port == SB_BASE + 8 || port == SB_BASE + 0) {
        g_opl.addr = v;
        g_opl.io_us++;
        return;
    }
    if (port == 0x389 || port == SB_BASE + 9 || port == SB_BASE + 1) {
        opl_write(g_opl.addr, v);
        return;
    }
    if (port == SB_BASE + 6) {
        if (v & 1)
            g_sb.reset_latch = 1;
        else if (g_sb.reset_latch) {
            g_sb.reset_latch = 0;
            sb_reset();
        }
        return;
    }
    if (port == SB_BASE + 0xC) {
        sb_write(v);
        return;
    }
}

static uint8_t audio_port_in_(uint16_t port);

uint8_t audio_port_in(uint16_t port)
{
    uint8_t v = audio_port_in_(port);
    if (rt_verbose >= 3) rt_log("audio in  %03x -> %02x", port, v);
    return v;
}

static uint8_t audio_port_in_(uint16_t port)
{
    if (port <= 0x0F) return dma_in(port);
    if (port >= 0x80 && port <= 0x8F) {
        int ch;
        for (ch = 0; ch < 4; ch++)
            if (k_page_port[ch] == port) return g_dma[ch].page;
        return 0xFF;
    }
    if (port == 0x388 || port == SB_BASE + 8 || port == SB_BASE + 0) {
        g_opl.io_us++;
        return opl_status();
    }
    if (port == SB_BASE + 0xA) {
        uint8_t v;
        if (g_sb.out_n == 0) return 0xFF;
        v = g_sb.out[0];
        memmove(g_sb.out, g_sb.out + 1, (size_t)--g_sb.out_n);
        return v;
    }
    if (port == SB_BASE + 0xC) return 0x7F; /* ready for a command */
    if (port == SB_BASE + 0xE) { /* data available; reading it acknowledges the interrupt */
        g_sb.irq_req = 0;
        return g_sb.out_n > 0 ? 0xFF : 0x7F;
    }
    return 0xFF;
}

/* ==== interrupt and mixing ============================================================================ */
int audio_irq(void)
{
    return SB_IRQ;
}

int audio_irq_pending(void)
{
    return g_sb.irq_req;
}

int audio_irq_take(void)
{
    if (!g_sb.irq_req) return 0;
    g_sb.irq_req = 0;
    return 1;
}

#define MIX_MAX 2048
static uint64_t g_mix_start, g_mix_done;
static int16_t g_mix[MIX_MAX * 2];
static int g_inited;

void audio_init(void)
{
    if (!g_inited) {
        opl_tables();
        g_inited = 1;
    }
    memset(g_dma, 0, sizeof g_dma);
    {
        int ch;
        for (ch = 0; ch < 4; ch++) g_dma[ch].masked = 1;
    }
    memset(&g_sb, 0, sizeof g_sb);
    memset(&g_opl, 0, sizeof g_opl);
    {
        int n;
        for (n = 0; n < 18; n++) {
            g_opl.op[n].env = 511;
            g_opl.op[n].stage = 3;
        }
    }
    g_opl.noise = 1;
    g_mix_start = host_time_us();
    g_mix_done = 0;
}

void audio_poll(void)
{
    int rate = host_audio_rate();
    uint64_t now, due;
    double opl_step, sb_step;
    if (!g_inited) audio_init();
    if (rate <= 0) return;
    now = host_time_us();
    due = (now - g_mix_start) * (uint64_t)rate / 1000000u;
    if (due <= g_mix_done) return;
    if (due - g_mix_done > (uint64_t)rate / 5u) g_mix_done = due - (uint64_t)rate / 5u; /* fell behind: skip */
    opl_step = OPL_RATE / rate;
    sb_step = g_sb.rate > 0 ? g_sb.rate / rate : 0;
    while (g_mix_done < due) {
        int n = (int)((due - g_mix_done) > MIX_MAX ? MIX_MAX : (due - g_mix_done)), i;
        for (i = 0; i < n; i++) {
            int32_t fm, dg, v;
            /* OPL at its own rate, linear interpolation to the output rate */
            g_opl.pos += opl_step;
            while (g_opl.pos >= 1.0) {
                int32_t s = opl_sample();
                g_opl.last = g_opl.cur;
                g_opl.cur = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
                g_opl.pos -= 1.0;
            }
            fm = g_opl.last + (int32_t)((g_opl.cur - g_opl.last) * g_opl.pos);
            /* Sound Blaster DMA at the DSP rate */
            if (g_sb.playing && !g_sb.paused && sb_step > 0) {
                g_sb.phase += sb_step;
                while (g_sb.phase >= 1.0) {
                    g_sb.prev = g_sb.cur;
                    g_sb.cur = sb_fetch();
                    g_sb.phase -= 1.0;
                }
                dg = g_sb.prev + (int32_t)((g_sb.cur - g_sb.prev) * g_sb.phase);
            } else {
                dg = (g_sb.playing || g_sb.cmd == 0x10) ? g_sb.cur : 0;
            }
            if (!g_sb.speaker) dg = 0;
            v = (fm + dg) * 3 / 4; /* headroom: nine FM voices plus full-scale samples */
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            g_mix[i * 2] = (int16_t)v;
            g_mix[i * 2 + 1] = (int16_t)v;
        }
        if (sb_step == 0 && g_sb.rate > 0) sb_step = g_sb.rate / rate;
        host_audio_push(g_mix, (size_t)n);
        g_mix_done += (uint64_t)n;
    }
}
