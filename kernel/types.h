/* NimbusOS - basic freestanding types (no libc headers are used). */
#ifndef NIMBUS_TYPES_H
#define NIMBUS_TYPES_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed char        i8;
typedef signed short       i16;
typedef signed int         i32;
typedef signed long long   i64;

typedef u8  uint8_t;
typedef u16 uint16_t;
typedef u32 uint32_t;
typedef u64 uint64_t;
typedef i8  int8_t;
typedef i16 int16_t;
typedef i32 int32_t;
typedef i64 int64_t;

typedef __SIZE_TYPE__    size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __UINTPTR_TYPE__ uintptr_t;
typedef int              ssize_t;

typedef _Bool bool;
#define true  1
#define false 0
#define NULL ((void *)0)

typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v)      __builtin_va_end(v)
#define va_arg(v, t)   __builtin_va_arg(v, t)

#define PACKED      __attribute__((packed))
#define NORETURN    __attribute__((noreturn))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

#endif
