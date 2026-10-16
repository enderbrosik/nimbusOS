/* NimbusOS libc - formatted output and console streams.
 * printf supports %d %i %u %x %X %o %p %s %c %%, the flags '-' '0' '+' ' ',
 * width and precision (also as '*'), and the l / ll / z length modifiers.
 * There is no floating point (user programs are built without SSE). */
#include <stdio.h>
#include <string.h>
#include <nimbus.h>

static FILE f_in = { 0 }, f_out = { 1 }, f_err = { 2 };
FILE *stdin = &f_in, *stdout = &f_out, *stderr = &f_err;

/* stdout is line buffered; stderr is unbuffered */
static char obuf[1024];
static size_t olen;

int fflush(FILE *f)
{
    (void)f;
    if (olen) { write(1, obuf, olen); olen = 0; }
    return 0;
}

int fputc(int c, FILE *f)
{
    if (f->fd == 2) { char ch = (char)c; fflush(stdout); write(2, &ch, 1); return c; }
    obuf[olen++] = (char)c;
    if (c == '\n' || olen == sizeof obuf) fflush(stdout);
    return c;
}

int putchar(int c) { return fputc(c, stdout); }

int fputs(const char *s, FILE *f)
{
    if (f->fd == 2) { fflush(stdout); write(2, s, strlen(s)); return 0; }
    while (*s) fputc(*s++, f);
    return 0;
}

int puts(const char *s) { fputs(s, stdout); fputc('\n', stdout); return 0; }

/* stdin: the Terminal sends whole lines (ending in '\n'). */
static char ibuf[512];
static size_t ipos, ilen;

static int fill(void)
{
    fflush(stdout);
    long n = read(0, ibuf, sizeof ibuf);
    if (n <= 0) return 0;
    ipos = 0; ilen = (size_t)n;
    return 1;
}

int getchar(void)
{
    if (ipos == ilen && !fill()) return EOF;
    return (unsigned char)ibuf[ipos++];
}

char *fgets(char *buf, int n, FILE *f)
{
    (void)f;
    int i = 0;
    while (i < n - 1) {
        int c = getchar();
        if (c == EOF) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    if (!i) return NULL;
    buf[i] = 0;
    return buf;
}

/* ---- formatting ---------------------------------------------------------- */

struct out { char *buf; size_t size, pos; FILE *f; };

static void put(struct out *o, char c)
{
    if (o->f) fputc(c, o->f);
    else if (o->pos + 1 < o->size) o->buf[o->pos] = c;
    o->pos++;
}

static void put_num(struct out *o, unsigned long long v, int base, int upper, int neg,
                    char sign, int width, int prec, int zero, int left)
{
    char tmp[24];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do { tmp[n++] = digits[v % base]; v /= base; } while (v);
    while (n < prec) tmp[n++] = '0';
    char s = neg ? '-' : sign;
    int len = n + (s ? 1 : 0);
    if (!left && !zero) while (width-- > len) put(o, ' ');
    if (s) put(o, s);
    if (!left && zero) while (width-- > len) put(o, '0');
    while (n) put(o, tmp[--n]);
    if (left) while (width-- > len) put(o, ' ');
}

static int format(struct out *o, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(o, *fmt); continue; }
        fmt++;
        int left = 0, zero = 0, width = 0, prec = -1, lng = 0;
        char sign = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') sign = '+';
            else if (*fmt == ' ') { if (!sign) sign = ' '; }
            else break;
        }
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z') { lng = 1; fmt++; }
        switch (*fmt) {
        case 'd': case 'i': {
            long long v = lng ? va_arg(ap, long long) : va_arg(ap, int);
            put_num(o, v < 0 ? -(unsigned long long)v : (unsigned long long)v, 10, 0, v < 0, sign,
                    width, prec, zero && prec < 0, left);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = lng ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
            int base = *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16;
            put_num(o, v, base, *fmt == 'X', 0, 0, width, prec, zero && prec < 0, left);
            break;
        }
        case 'p':
            put(o, '0'); put(o, 'x');
            put_num(o, (unsigned long long)va_arg(ap, void *), 16, 0, 0, 0, 0, -1, 0, 0);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)strlen(s);
            if (prec >= 0 && len > prec) len = prec;
            if (!left) while (width-- > len) put(o, ' ');
            for (int i = 0; i < len; i++) put(o, s[i]);
            if (left) while (width-- > len) put(o, ' ');
            break;
        }
        case 'c': {
            if (!left) while (width-- > 1) put(o, ' ');
            put(o, (char)va_arg(ap, int));
            if (left) while (width-- > 1) put(o, ' ');
            break;
        }
        case '%': put(o, '%'); break;
        case 0: fmt--; break;
        default: put(o, '%'); put(o, *fmt); break;
        }
    }
    return (int)o->pos;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    struct out o = { buf, n, 0, NULL };
    int r = format(&o, fmt, ap);
    if (n) buf[o.pos < n ? o.pos : n - 1] = 0;
    return r;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    if (f->fd == 2) {
        char buf[512];
        int r = vsnprintf(buf, sizeof buf, fmt, ap);
        fputs(buf, f);
        return r;
    }
    struct out o = { NULL, 0, 0, f };
    return format(&o, fmt, ap);
}

int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int printf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, (size_t)-1 >> 1, fmt, ap);
    va_end(ap);
    return r;
}
