/* NimbusOS - minimal printf family. Supports %d %i %u %x %X %p %s %c %%,
 * flags '-' '0', width, precision for strings, and 'l'/'ll' lengths. */
#include "kernel/kprintf.h"
#include "kernel/string.h"
#include "drivers/serial.h"
#include "drivers/vga_text.h"
#include "arch/x86/io.h"

struct out { char *buf; size_t size, pos; };

static void put(struct out *o, char c)
{
    if (o->pos + 1 < o->size) o->buf[o->pos] = c;
    o->pos++;
}

static void put_num(struct out *o, u64 v, int base, bool upper, bool neg, int width, bool zero, bool left)
{
    char tmp[24];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do { tmp[n++] = digits[v % base]; v /= base; } while (v);
    int len = n + (neg ? 1 : 0);
    if (!left && !zero) while (width-- > len) put(o, ' ');
    if (neg) put(o, '-');
    if (!left && zero) while (width-- > len) put(o, '0');
    while (n) put(o, tmp[--n]);
    if (left) while (width-- > len) put(o, ' ');
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&o, *fmt); continue; }
        fmt++;
        bool left = false, zero = false;
        int width = 0, prec = -1, lng = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else break;
        }
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            while (isdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z') { lng = 2; fmt++; }        /* size_t */
        if (lng == 1 && sizeof(long) == 8) lng = 2;
        switch (*fmt) {
        case 'd': case 'i': {
            i64 v = lng >= 2 ? va_arg(ap, i64) : (i64)va_arg(ap, int);
            put_num(&o, v < 0 ? -(u64)v : (u64)v, 10, false, v < 0, width, zero, left);
            break;
        }
        case 'u': case 'x': case 'X': {
            u64 v = lng >= 2 ? va_arg(ap, u64) : (u64)va_arg(ap, unsigned);
            put_num(&o, v, *fmt == 'u' ? 10 : 16, *fmt == 'X', false, width, zero, left);
            break;
        }
        case 'p':
            put(&o, '0'); put(&o, 'x');
        {
            u64 v = (uptr)va_arg(ap, void *);
            put_num(&o, v, 16, false, false, v >> 32 ? 16 : 8, true, false);
        }
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = strlen(s);
            if (prec >= 0 && len > prec) len = prec;
            if (!left) while (width-- > len) put(&o, ' ');
            for (int i = 0; i < len; i++) put(&o, s[i]);
            if (left) while (width-- > len) put(&o, ' ');
            break;
        }
        case 'c': put(&o, (char)va_arg(ap, int)); break;
        case '%': put(&o, '%'); break;
        case 0: fmt--; break;
        default: put(&o, '%'); put(&o, *fmt); break;
        }
    }
    if (size) buf[o.pos < size ? o.pos : size - 1] = 0;
    return (int)o.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

void kprintf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    u32 f = irq_save();
    serial_write(buf);
    vga_write(buf);
    irq_restore(f);
}

void klog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    u32 f = irq_save();
    serial_write(buf);
    irq_restore(f);
}
