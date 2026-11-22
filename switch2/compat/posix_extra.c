// POSIX functions newlib (devkitA64's C library) lacks, which DXC's LLVM support library
// and the runtime refer to (declared in compat/include and newlib's headers). Anonymous
// memory mappings come from the heap; positioned reads and writes seek under a lock; the
// rest report failure or do nothing, which their callers handle.
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <unistd.h>

void* mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset)
{
    (void)protection;
    (void)fd;
    (void)offset;
    if ((flags & MAP_ANONYMOUS) == 0 || (flags & MAP_FIXED) != 0)
    {
        (void)address;
        errno = ENOSYS;
        return MAP_FAILED;
    }
    size_t size = (length + 0xFFF) & ~(size_t)0xFFF;
    void* memory = aligned_alloc(0x1000, size);
    if (memory == NULL)
    {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    memset(memory, 0, size);
    return memory;
}

int munmap(void* address, size_t length)
{
    (void)length;
    free(address);
    return 0;
}

int mprotect(void* address, size_t length, int protection)
{
    (void)address;
    (void)length;
    (void)protection;
    return 0;
}

int msync(void* address, size_t length, int flags)
{
    (void)address;
    (void)length;
    (void)flags;
    return 0;
}

void* dlopen(const char* file, int mode)
{
    (void)file;
    (void)mode;
    return NULL;
}

void* dlsym(void* handle, const char* name)
{
    (void)handle;
    (void)name;
    return NULL;
}

int dlclose(void* handle)
{
    (void)handle;
    return 0;
}

char* dlerror(void)
{
    return (char*)"no dynamic loader";
}

int dladdr(const void* address, Dl_info* info)
{
    (void)address;
    (void)info;
    return 0;
}

int getpagesize(void)
{
    return 0x1000;
}

int uname(struct utsname* name)
{
    memset(name, 0, sizeof(*name));
    strcpy(name->sysname, "Horizon");
    strcpy(name->machine, "aarch64");
    return 0;
}

// pread and pwrite: seek, transfer and seek back, under one lock, so threads sharing a file
// don't move each other's position.
static pthread_mutex_t g_positionMutex = PTHREAD_MUTEX_INITIALIZER;

static ssize_t TransferAt(int fd, void* buffer, size_t count, off_t offset, int writing)
{
    pthread_mutex_lock(&g_positionMutex);
    ssize_t result = -1;
    off_t previous = lseek(fd, 0, SEEK_CUR);
    if (previous >= 0 && lseek(fd, offset, SEEK_SET) == offset)
    {
        result = writing ? (ssize_t)write(fd, buffer, count) : read(fd, buffer, count);
        int error = errno;
        lseek(fd, previous, SEEK_SET);
        errno = error;
    }
    pthread_mutex_unlock(&g_positionMutex);
    return result;
}

ssize_t pread(int fd, void* buffer, size_t count, off_t offset)
{
    return TransferAt(fd, buffer, count, offset, 0);
}

ssize_t pwrite(int fd, const void* buffer, size_t count, off_t offset)
{
    return TransferAt(fd, (void*)buffer, count, offset, 1);
}

// No signals: faults go to libnx's exception handler. LLVM's crash handler, which these
// install, never runs.
int sigaction(int signal, const struct sigaction* action, struct sigaction* previous)
{
    (void)signal;
    (void)action;
    if (previous != NULL)
        memset(previous, 0, sizeof(*previous));
    return 0;
}

int sigprocmask(int how, const sigset_t* set, sigset_t* previous)
{
    (void)how;
    (void)set;
    if (previous != NULL)
        memset(previous, 0, sizeof(*previous));
    return 0;
}
