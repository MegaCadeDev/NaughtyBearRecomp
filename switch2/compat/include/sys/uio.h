// newlib has no sys/uio.h; LLVM's virtual file system includes it but uses nothing from it.
#pragma once
#include <stddef.h>
#include <sys/types.h>

struct iovec
{
    void* iov_base;
    size_t iov_len;
};
