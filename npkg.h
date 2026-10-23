#ifndef NIMBUS_NPKG_H
#define NIMBUS_NPKG_H

#ifdef NIMBUS_KERNEL
#include "kernel/types.h"
typedef u8 npkg_u8;
typedef u32 npkg_u32;
typedef u64 npkg_u64;
#else
#include <stdint.h>
typedef uint8_t npkg_u8;
typedef uint32_t npkg_u32;
typedef uint64_t npkg_u64;
#endif

#define NPKG_MAGIC "NIMPKG1"
#define NPKG_MAGIC_SIZE 8
#define NPKG_VERSION 1
#define NPKG_NAME_MAX 48

struct npkg_header {
    npkg_u8 magic[NPKG_MAGIC_SIZE];
    npkg_u32 version;
    npkg_u32 name_len;
    npkg_u64 elf_size;
} __attribute__((packed));

#endif