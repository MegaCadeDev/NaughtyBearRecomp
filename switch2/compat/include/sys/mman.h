// newlib has no sys/mman.h. The Switch 2 runtime and DXC only need anonymous mappings,
// which posix_extra.c makes from aligned heap memory; file mappings fail, and callers
// (LLVM's MemoryBuffer) read the file instead.
#pragma once
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_FAILED ((void*)-1)
#define MS_ASYNC 1
#define MS_INVALIDATE 2
#define MS_SYNC 4

void* mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset);
int munmap(void* address, size_t length);
int mprotect(void* address, size_t length, int protection);
int msync(void* address, size_t length, int flags);

#ifdef __cplusplus
}
#endif
