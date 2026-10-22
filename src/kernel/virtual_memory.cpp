// NtAllocateVirtualMemory and friends for Naughty Bear.
//
// Unleashed Recompiled leaves these as stubs because it replaces Sonic's CRT heap by
// address. Naughty Bear's own heaps run as guest code and reserve their memory here, so
// this is a real (if simple) virtual allocator over 0x40000000-0x7FEA0000.
//
// Guest memory is one host mapping that is always readable and writable, so committing
// is bookkeeping only. Released and decommitted ranges are zeroed, because Windows-style
// virtual memory hands out zero-filled pages.

#include <stdafx.h>
#include "function.h"
#include "memory.h"
#include "xdm.h"
#include <map>
#include <os/logger.h>

namespace
{
    constexpr uint32_t VIRTUAL_BEGIN = 0x40000000;
    constexpr uint32_t VIRTUAL_END = 0x7FEA0000;
    constexpr uint32_t ALLOCATION_GRANULARITY = 0x10000;
    constexpr uint32_t PAGE_SIZE_4K = 0x1000;

    constexpr uint32_t X_MEM_COMMIT = 0x1000;
    constexpr uint32_t X_MEM_RESERVE = 0x2000;
    constexpr uint32_t X_MEM_DECOMMIT = 0x4000;
    constexpr uint32_t X_MEM_RELEASE = 0x8000;
    constexpr uint32_t X_MEM_FREE = 0x10000;
    constexpr uint32_t X_MEM_PRIVATE = 0x20000;
    constexpr uint32_t X_MEM_TOP_DOWN = 0x100000;
    constexpr uint32_t X_MEM_LARGE_PAGES = 0x20000000;

    constexpr uint32_t X_STATUS_INVALID_PARAMETER = 0xC000000D;
    constexpr uint32_t X_STATUS_NO_MEMORY = 0xC0000017;
    constexpr uint32_t X_STATUS_MEMORY_NOT_ALLOCATED = 0xC00000A0;

    struct Region
    {
        uint32_t size;
        uint32_t protect;
    };

    Mutex g_virtualMutex;
    std::map<uint32_t, Region> g_regions; // reservation base -> region

    void ZeroRange(uint32_t address, uint32_t size)
    {
        // Dropping the pages makes the private anonymous mapping read back as zeroes.
        madvise(g_memory.Translate(address), size, MADV_DONTNEED);
    }

    // Finds the reservation containing address, or end().
    std::map<uint32_t, Region>::iterator FindRegion(uint32_t address)
    {
        auto it = g_regions.upper_bound(address);
        if (it == g_regions.begin())
            return g_regions.end();
        --it;
        return address < it->first + it->second.size ? it : g_regions.end();
    }

    bool IsRangeFree(uint32_t base, uint32_t size)
    {
        auto it = g_regions.lower_bound(base);
        if (it != g_regions.end() && it->first < base + size)
            return false;
        if (it != g_regions.begin())
        {
            --it;
            if (it->first + it->second.size > base)
                return false;
        }
        return true;
    }

    std::optional<uint32_t> FindFreeRange(uint32_t size, bool topDown)
    {
        if (topDown)
        {
            uint32_t end = VIRTUAL_END & ~(ALLOCATION_GRANULARITY - 1);
            for (auto it = g_regions.rbegin(); ; ++it)
            {
                uint32_t gapStart = it == g_regions.rend() ? VIRTUAL_BEGIN : RoundUp(it->first + it->second.size, ALLOCATION_GRANULARITY);
                if (end >= gapStart + size && end - size >= VIRTUAL_BEGIN)
                    return (end - size) & ~(ALLOCATION_GRANULARITY - 1);
                if (it == g_regions.rend())
                    return std::nullopt;
                end = it->first;
            }
        }

        uint32_t start = VIRTUAL_BEGIN;
        for (const auto& [base, region] : g_regions)
        {
            if (base >= start + size)
                return start;
            start = std::max(start, RoundUp(base + region.size, ALLOCATION_GRANULARITY));
        }
        if (start + size <= VIRTUAL_END)
            return start;
        return std::nullopt;
    }
}

uint32_t NtAllocateVirtualMemory(be<uint32_t>* baseAddress, be<uint32_t>* regionSize, uint32_t allocationType, uint32_t protect, uint32_t debugMemory)
{
    if (baseAddress == nullptr || regionSize == nullptr || (allocationType & (X_MEM_COMMIT | X_MEM_RESERVE)) == 0)
        return X_STATUS_INVALID_PARAMETER;

    uint32_t pageSize = (allocationType & X_MEM_LARGE_PAGES) ? ALLOCATION_GRANULARITY : PAGE_SIZE_4K;
    uint32_t requested = *baseAddress;
    uint32_t size = RoundUp(uint32_t(*regionSize), pageSize);
    if (size == 0)
        return X_STATUS_INVALID_PARAMETER;

    std::lock_guard lock(g_virtualMutex);

    if (requested != 0)
    {
        // Committing inside an existing reservation needs no work: guest memory is always mapped.
        auto it = FindRegion(requested);
        if (it != g_regions.end())
        {
            uint32_t begin = requested & ~(pageSize - 1);
            if (begin + size > it->first + it->second.size)
                return X_STATUS_INVALID_PARAMETER;
            *baseAddress = begin;
            *regionSize = size;
            return STATUS_SUCCESS;
        }

        // Reserving at a fixed address.
        uint32_t base = requested & ~(ALLOCATION_GRANULARITY - 1);
        size = RoundUp(requested + size, pageSize) - base;
        if (base < VIRTUAL_BEGIN || base + size > VIRTUAL_END || !IsRangeFree(base, size))
            return X_STATUS_NO_MEMORY;

        g_regions[base] = { size, protect };
        *baseAddress = base;
        *regionSize = size;
        return STATUS_SUCCESS;
    }

    auto base = FindFreeRange(RoundUp(size, ALLOCATION_GRANULARITY), (allocationType & X_MEM_TOP_DOWN) != 0);
    if (!base)
    {
        LOGF_ERROR("Out of virtual memory allocating 0x{:X} bytes", size);
        return X_STATUS_NO_MEMORY;
    }

    g_regions[*base] = { size, protect };
    *baseAddress = *base;
    *regionSize = size;
    return STATUS_SUCCESS;
}

uint32_t NtFreeVirtualMemory(be<uint32_t>* baseAddress, be<uint32_t>* regionSize, uint32_t freeType, uint32_t debugMemory)
{
    if (baseAddress == nullptr || regionSize == nullptr)
        return X_STATUS_INVALID_PARAMETER;

    std::lock_guard lock(g_virtualMutex);

    auto it = FindRegion(*baseAddress);
    if (it == g_regions.end())
        return X_STATUS_MEMORY_NOT_ALLOCATED;

    if (freeType & X_MEM_RELEASE)
    {
        ZeroRange(it->first, it->second.size);
        *baseAddress = it->first;
        *regionSize = it->second.size;
        g_regions.erase(it);
        return STATUS_SUCCESS;
    }

    if (freeType & X_MEM_DECOMMIT)
    {
        uint32_t begin = *baseAddress & ~(PAGE_SIZE_4K - 1);
        uint32_t size = *regionSize == 0 ? it->first + it->second.size - begin : RoundUp(uint32_t(*regionSize), PAGE_SIZE_4K);
        size = std::min(size, it->first + it->second.size - begin);
        ZeroRange(begin, size);
        *baseAddress = begin;
        *regionSize = size;
        return STATUS_SUCCESS;
    }

    return X_STATUS_INVALID_PARAMETER;
}

struct X_MEMORY_BASIC_INFORMATION
{
    be<uint32_t> BaseAddress;
    be<uint32_t> AllocationBase;
    be<uint32_t> AllocationProtect;
    be<uint32_t> RegionSize;
    be<uint32_t> State;
    be<uint32_t> Protect;
    be<uint32_t> Type;
};

uint32_t NtQueryVirtualMemory(uint32_t baseAddress, X_MEMORY_BASIC_INFORMATION* info)
{
    if (info == nullptr)
        return X_STATUS_INVALID_PARAMETER;

    std::lock_guard lock(g_virtualMutex);

    memset(info, 0, sizeof(*info));
    uint32_t page = baseAddress & ~(PAGE_SIZE_4K - 1);
    auto it = FindRegion(page);
    if (it != g_regions.end())
    {
        info->BaseAddress = page;
        info->AllocationBase = it->first;
        info->AllocationProtect = it->second.protect;
        info->RegionSize = it->first + it->second.size - page;
        info->State = X_MEM_COMMIT;
        info->Protect = it->second.protect;
        info->Type = X_MEM_PRIVATE;
    }
    else
    {
        info->BaseAddress = page;
        info->RegionSize = PAGE_SIZE_4K;
        info->State = X_MEM_FREE;
    }
    return STATUS_SUCCESS;
}

GUEST_FUNCTION_HOOK(__imp__NtAllocateVirtualMemory, NtAllocateVirtualMemory);
GUEST_FUNCTION_HOOK(__imp__NtFreeVirtualMemory, NtFreeVirtualMemory);
GUEST_FUNCTION_HOOK(__imp__NtQueryVirtualMemory, NtQueryVirtualMemory);
