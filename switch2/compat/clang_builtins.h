// The Clang builtins XenonRecomp's generated code and the runtime use, for GCC: the Switch 2
// build uses devkitA64's GCC throughout, because libnx's thread-local storage
// (-mtp=soft) is GCC's. Every Switch 2 source includes this first (switch2/CMakeLists.txt).
#pragma once

#if !defined(__clang__)

#include <stdint.h>

static inline uint32_t nb_rotateleft32(uint32_t value, uint32_t shift)
{
    shift &= 31;
    return shift == 0 ? value : (value << shift) | (value >> (32 - shift));
}

static inline uint64_t nb_rotateleft64(uint64_t value, uint64_t shift)
{
    shift &= 63;
    return shift == 0 ? value : (value << shift) | (value >> (64 - shift));
}

#define __builtin_rotateleft32(value, shift) nb_rotateleft32((value), (shift))
#define __builtin_rotateleft64(value, shift) nb_rotateleft64((value), (shift))
#define __builtin_assume(condition) do { if (!(condition)) __builtin_unreachable(); } while (0)
#define __builtin_debugtrap() __builtin_trap()

#endif
