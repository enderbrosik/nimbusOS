#ifndef NIMBUS_KPRINTF_H
#define NIMBUS_KPRINTF_H
#include "kernel/types.h"
int  vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int  snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));  /* console + serial */
void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));     /* serial only */
#endif
