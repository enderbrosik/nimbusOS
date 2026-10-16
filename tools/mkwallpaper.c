/*
 * NimbusOS - generates the stock wallpapers and sample pictures as 24-bit
 * uncompressed BMP files (procedurally, no external assets):
 *
 *   mkwallpaper <out-dir>
 *     <out-dir>/System/wallpaper.bmp            default wallpaper ("Hills")
 *     <out-dir>/System/Wallpapers/Hills.bmp
 *     <out-dir>/System/Wallpapers/Sunset.bmp
 *     <out-dir>/System/Wallpapers/Aurora.bmp
 *     <out-dir>/Pictures/Sample.bmp             a small picture for Paint
 *     <out-dir>/Music/Ode to Joy.wav             a short synthesised tune (16-bit PCM)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

typedef struct { int w, h; uint8_t *px; } img_t;   /* RGB, top-down */

static img_t img_new(int w, int h) { img_t i = { w, h, calloc(w * h, 3) }; return i; }
static void put(img_t *im, int x, int y, double r, double g, double b)
{
    if (x < 0 || y < 0 || x >= im->w || y >= im->h) return;
    uint8_t *p = im->px + (y * im->w + x) * 3;
    p[0] = r < 0 ? 0 : r > 255 ? 255 : r;
    p[1] = g < 0 ? 0 : g > 255 ? 255 : g;
    p[2] = b < 0 ? 0 : b > 255 ? 255 : b;
}
static double mix(double a, double b, double t) { return a + (b - a) * t; }
static double clamp01(double t) { return t < 0 ? 0 : t > 1 ? 1 : t; }

static void save_bmp(const char *path, img_t *im)
{
    int row = (im->w * 3 + 3) & ~3;
    uint32_t size = 54 + row * im->h;
    uint8_t hdr[54] = { 'B', 'M' };
#define W32(o, v) do { uint32_t _v = (v); memcpy(hdr + (o), &_v, 4); } while (0)
#define W16(o, v) do { uint16_t _v = (v); memcpy(hdr + (o), &_v, 2); } while (0)
    W32(2, size); W32(10, 54); W32(14, 40); W32(18, im->w); W32(22, im->h);
    W16(26, 1); W16(28, 24); W32(34, row * im->h); W32(38, 2835); W32(42, 2835);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(hdr, 1, 54, f);
    uint8_t *line = calloc(row, 1);
    for (int y = im->h - 1; y >= 0; y--) {          /* bottom-up rows, BGR */
        for (int x = 0; x < im->w; x++) {
            uint8_t *p = im->px + (y * im->w + x) * 3;
            line[x * 3] = p[2]; line[x * 3 + 1] = p[1]; line[x * 3 + 2] = p[0];
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
}

static void hills(img_t *im)
{
    int W = im->w, H = im->h;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            double t = (double)y / H;
            put(im, x, y, mix(70, 170, t), mix(130, 210, t), mix(210, 240, t));
        }
    /* sun with glow */
    double sx = W * 0.72, sy = H * 0.28;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            double d = hypot(x - sx, y - sy);
            uint8_t *p = im->px + (y * W + x) * 3;
            double glow = clamp01(1.0 - d / 260.0);
            glow = glow * glow * 0.6;
            double r = mix(p[0], 255, glow), g = mix(p[1], 240, glow), b = mix(p[2], 200, glow);
            if (d < 48) { r = 255; g = 244; b = 214; }
            put(im, x, y, r, g, b);
        }
    /* three layers of rolling hills */
    struct { double base, a1, f1, a2, f2, ph; double r, g, b; } L[3] = {
        { 0.62, 40, 0.004, 18, 0.011, 0.3, 120, 175, 110 },
        { 0.72, 35, 0.006, 15, 0.017, 1.9, 70, 150, 80 },
        { 0.84, 30, 0.005, 12, 0.021, 4.1, 40, 120, 60 },
    };
    for (int l = 0; l < 3; l++)
        for (int x = 0; x < W; x++) {
            double top = H * L[l].base + L[l].a1 * sin(x * L[l].f1 + L[l].ph) + L[l].a2 * sin(x * L[l].f2 + L[l].ph * 2);
            for (int y = (int)top; y < H; y++) {
                double shade = clamp01((y - top) / (H * 0.4));
                put(im, x, y, L[l].r * (1 - 0.35 * shade), L[l].g * (1 - 0.35 * shade), L[l].b * (1 - 0.35 * shade));
            }
        }
}

static void sunset(img_t *im)
{
    int W = im->w, H = im->h;
    double horizon = H * 0.62;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            if (y < horizon) {
                double t = y / horizon;
                put(im, x, y, mix(40, 250, t), mix(30, 140, t * t), mix(90, 90, t));
            } else {
                double t = (y - horizon) / (H - horizon);
                double ripple = 0.5 + 0.5 * sin(y * 0.9 + sin(x * 0.02) * 3);
                double sun_reflect = clamp01(1 - fabs(x - W * 0.5) / (60 + t * 160)) * (ripple > 0.6);
                put(im, x, y, mix(mix(120, 20, t), 255, sun_reflect * 0.8), mix(mix(70, 30, t), 200, sun_reflect * 0.8),
                    mix(mix(110, 60, t), 120, sun_reflect * 0.5));
            }
        }
    for (int y = 0; y < (int)horizon; y++)
        for (int x = 0; x < W; x++) {
            double d = hypot(x - W * 0.5, y - horizon);
            if (d < 90) put(im, x, y, 255, mix(230, 170, d / 90), mix(150, 80, d / 90));
        }
}

static void aurora(img_t *im)
{
    int W = im->w, H = im->h;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            double t = (double)y / H;
            double r = mix(5, 20, t), g = mix(10, 30, t), b = mix(30, 60, t);
            for (int band = 0; band < 3; band++) {
                double cy = H * (0.25 + band * 0.12) + 60 * sin(x * 0.006 + band * 2.0) + 25 * sin(x * 0.017 + band);
                double d = (y - cy) / (40.0 + band * 15);
                double a = exp(-d * d) * (0.55 + 0.45 * sin(x * 0.05 + band));
                r += a * (band == 2 ? 120 : 20);
                g += a * (band == 1 ? 140 : 220);
                b += a * (band == 0 ? 120 : 160);
            }
            put(im, x, y, r, g, b);
        }
    srand(7);
    for (int i = 0; i < 400; i++) {
        int x = rand() % W, y = rand() % (H * 2 / 3);
        double v = 150 + rand() % 105;
        put(im, x, y, v, v, v);
    }
    for (int x = 0; x < W; x++) {        /* mountain silhouette */
        double top = H * 0.8 - 90 * fabs(sin(x * 0.004 + 1)) - 40 * fabs(sin(x * 0.013));
        for (int y = (int)top; y < H; y++) put(im, x, y, 8, 12, 20);
    }
}

static void sample(img_t *im)
{
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) put(im, x, y, 250, 250, 245);
    for (int y = 40; y < 140; y++)
        for (int x = 30; x < 130; x++) put(im, x, y, 220, 60, 60);
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            if (hypot(x - 200, y - 110) < 60) put(im, x, y, 60, 120, 230);
            if (y > 170 && y < 190 && x > 30 && x < 290) put(im, x, y, 40, 170, 80);
        }
}

/* A short tune: the opening of Beethoven's "Ode to Joy" (public domain),
 * a soft organ-like tone (fundamental + 2 harmonics) over a bass note. */
static void save_tune(const char *path)
{
    static const char *mel = "EEFGGFEDCCDEEDD-EEFGGFEDCCDEDCC-";
    static const double hz[7] = { 261.63, 293.66, 329.63, 349.23, 392.00, 440.00, 493.88 };   /* C..B */
    const int rate = 22050, beat = rate * 30 / 100;    /* 0.3 s per note */
    int n = (int)strlen(mel), frames = n * beat + rate / 2;
    int16_t *pcm = calloc(frames, 2);
    for (int i = 0; i < n; i++) {
        if (mel[i] == '-') continue;
        double f = hz[mel[i] - 'C'];
        double hold = (i + 1 < n && mel[i + 1] == '-') ? 2.0 : 1.0;
        int len = (int)(beat * hold);
        for (int k = 0; k < len + rate / 10; k++) {
            int idx = i * beat + k;
            if (idx >= frames) break;
            double t = (double)k / rate;
            double env = k < 300 ? k / 300.0 : exp(-2.2 * t);
            if (k > len) env *= 1.0 - (double)(k - len) / (rate / 10);
            double v = sin(2 * M_PI * f * t) + 0.35 * sin(4 * M_PI * f * t) + 0.15 * sin(6 * M_PI * f * t);
            double bass = 0.45 * sin(2 * M_PI * (f / 2) * t);
            double s = pcm[idx] + 6000.0 * env * (v + bass);
            pcm[idx] = s > 32767 ? 32767 : s < -32768 ? -32768 : (int16_t)s;
        }
    }
    uint8_t h[44] = { 'R', 'I', 'F', 'F' };
    uint32_t dbytes = frames * 2;
#define P32(o, v) do { uint32_t _v = (v); memcpy(h + (o), &_v, 4); } while (0)
#define P16(o, v) do { uint16_t _v = (v); memcpy(h + (o), &_v, 2); } while (0)
    P32(4, 36 + dbytes); memcpy(h + 8, "WAVEfmt ", 8); P32(16, 16); P16(20, 1); P16(22, 1);
    P32(24, rate); P32(28, rate * 2); P16(32, 2); P16(34, 16); memcpy(h + 36, "data", 4); P32(40, dbytes);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(h, 1, 44, f);
    fwrite(pcm, 2, frames, f);
    fclose(f);
    free(pcm);
}

static void mkdirs(const char *base, const char *sub)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", base); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/%s", base, sub); mkdir(p, 0755);
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: mkwallpaper <out-dir>\n"); return 1; }
    const char *o = argv[1];
    char p[1024];
    mkdirs(o, "System");
    mkdirs(o, "System/Wallpapers");
    mkdirs(o, "Pictures");
    mkdirs(o, "Music");
    snprintf(p, sizeof p, "%s/Music/Ode to Joy.wav", o); save_tune(p);
    img_t a = img_new(1024, 768), b = img_new(1024, 768), c = img_new(1024, 768), s = img_new(320, 240);
    hills(&a); sunset(&b); aurora(&c); sample(&s);
    snprintf(p, sizeof p, "%s/System/wallpaper.bmp", o); save_bmp(p, &a);
    snprintf(p, sizeof p, "%s/System/Wallpapers/Hills.bmp", o); save_bmp(p, &a);
    snprintf(p, sizeof p, "%s/System/Wallpapers/Sunset.bmp", o); save_bmp(p, &b);
    snprintf(p, sizeof p, "%s/System/Wallpapers/Aurora.bmp", o); save_bmp(p, &c);
    snprintf(p, sizeof p, "%s/Pictures/Sample.bmp", o); save_bmp(p, &s);
    return 0;
}
