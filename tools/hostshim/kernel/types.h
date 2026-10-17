/* Host build of kernel code for tools/test_imgdec.py: just enough of the kernel headers. */
#ifndef HOSTSHIM_TYPES_H
#define HOSTSHIM_TYPES_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t i8; typedef int16_t i16; typedef int32_t i32; typedef int64_t i64;
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
