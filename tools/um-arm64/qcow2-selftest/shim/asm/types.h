/* SPDX-License-Identifier: GPL-2.0 */
/* Host shim: qcow2.h asks for <asm/types.h>; off-UML that is stdint. */
#ifndef __SHIM_ASM_TYPES_H__
#define __SHIM_ASM_TYPES_H__

#include <stdint.h>

typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;

typedef int8_t   __s8;
typedef int16_t  __s16;
typedef int32_t  __s32;
typedef int64_t  __s64;

#endif /* __SHIM_ASM_TYPES_H__ */