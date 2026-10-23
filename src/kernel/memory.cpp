#include <stdafx.h>
#include "memory.h"
#if defined(__PROSPERO__)
#include <ps5platform/shm.h>
#endif

Memory::Memory()
{
#ifdef _WIN32
    base = (uint8_t*)VirtualAlloc((void*)0x100000000ull, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (base == nullptr)
        base = (uint8_t*)VirtualAlloc(nullptr, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (base == nullptr)
        return;

    DWORD oldProtect;
    VirtualProtect(base, 4096, PAGE_NOACCESS, &oldProtect);
#elif defined(__PROSPERO__)
    // PS5 (after UnleashedRecomp-Prospero, GPL-3.0): anonymous mmap can't provide 4 GB, so
    // the guest space is shared memory at a fixed address. If 4 GB in one piece isn't
    // available, only the ranges the runtime uses are mapped: the heaps (to 0x7FF00000),
    // the executable and its function table (0x82000000-0x88000000) and physical memory with
    // its overflow (0xA0000000 up).
    void* view = nullptr;
    struct ps5_shm shm{};
    if (ps5_shm_create(PPC_MEMORY_SIZE, &shm) == 0 &&
        ps5_shm_map(&shm, 0, PPC_MEMORY_SIZE, (void*)0x1000000000ull, PS5_SHM_READ | PS5_SHM_WRITE, 0, &view) == 0)
    {
        base = (uint8_t*)view;
    }
    else if (ps5_vrange_reserve(PPC_MEMORY_SIZE, (void*)0x1000000000ull, 0x10000, &view) == 0 && view != nullptr)
    {
        const std::pair<uint64_t, uint64_t> ranges[] = { { 0x10000, 0x7FEF0000 }, { 0x82000000, 0x06000000 }, { 0xA0000000, 0x60000000 } };
        for (auto [offset, size] : ranges)
        {
            struct ps5_shm part{};
            void* mapped = nullptr;
            if (ps5_shm_create(size, &part) != 0 ||
                ps5_shm_map(&part, 0, size, (uint8_t*)view + offset, PS5_SHM_READ | PS5_SHM_WRITE, PS5_SHM_FIXED, &mapped) != 0)
                return;
        }
        base = (uint8_t*)view;
    }

    if (base == nullptr)
        return;

    mprotect(base, 4096, PROT_NONE);
#else
    base = (uint8_t*)mmap((void*)0x100000000ull, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

    if (base == (uint8_t*)MAP_FAILED)
        base = (uint8_t*)mmap(NULL, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

    if (base == nullptr)
        return;

    mprotect(base, 4096, PROT_NONE);
#endif

    for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++)
    {
        if (PPCFuncMappings[i].host != nullptr)
            InsertFunction(PPCFuncMappings[i].guest, PPCFuncMappings[i].host);
    }
}

void* MmGetHostAddress(uint32_t ptr)
{
    return g_memory.Translate(ptr);
}
