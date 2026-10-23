#if !defined(__PROSPERO__) // the PS5 version has no mods

#include <stdafx.h>
#include "origins.h"
#include <kernel/function.h>
#include <map>

namespace origins
{
    namespace
    {
        struct Range
        {
            uint32_t end;
            uint32_t name; // index in g_names
        };

        std::mutex g_mutex;
        std::atomic<bool> g_any = false;
        std::map<uint32_t, Range> g_ranges; // by start; ranges don't overlap
        std::vector<std::string> g_names;

        const Range* FindRange(uint32_t address, uint32_t& start)
        {
            auto it = g_ranges.upper_bound(address);
            if (it == g_ranges.begin())
                return nullptr;
            --it;
            if (address >= it->second.end)
                return nullptr;
            start = it->first;
            return &it->second;
        }

        // Records [start, end) as coming from a package, replacing what was there: the
        // memory has been reused.
        void Insert(uint32_t start, uint32_t end, uint32_t name)
        {
            if (start >= end)
                return;
            auto it = g_ranges.lower_bound(start);
            if (it != g_ranges.begin())
            {
                auto previous = std::prev(it);
                if (previous->second.end > start)
                {
                    Range tail = previous->second;
                    previous->second.end = start;
                    if (tail.end > end)
                        g_ranges[end] = { tail.end, tail.name };
                }
            }
            for (it = g_ranges.lower_bound(start); it != g_ranges.end() && it->first < end;)
            {
                if (it->second.end > end)
                {
                    Range tail = it->second;
                    g_ranges.erase(it);
                    g_ranges[end] = tail;
                    break;
                }
                it = g_ranges.erase(it);
            }
            g_ranges[start] = { end, name };
        }
    }

    void NoteRead(const std::filesystem::path& file, uint32_t address, uint32_t size)
    {
        if (file.extension() != ".lu" || size == 0)
            return;
        std::string name = file.stem().string();
        std::lock_guard lock(g_mutex);
        auto it = std::find(g_names.begin(), g_names.end(), name);
        uint32_t index = uint32_t(it - g_names.begin());
        if (it == g_names.end())
            g_names.push_back(name);
        Insert(address, address + size, index);
        g_any = true;
    }

    void NoteDecompress(uint32_t source, uint32_t destination, uint32_t size)
    {
        std::lock_guard lock(g_mutex);
        uint32_t start;
        const Range* range = FindRange(source, start);
        if (range != nullptr)
            Insert(destination, destination + size, range->name);
    }

    void NoteCopy(uint32_t source, uint32_t destination, uint32_t size)
    {
        // Small copies are too frequent to look up, and aren't mesh data.
        if (size < 4096 || !g_any)
            return;
        std::lock_guard lock(g_mutex);
        uint32_t start;
        if (const Range* range = FindRange(source, start))
            Insert(destination, destination + size, range->name);
    }

    std::string Find(uint32_t address)
    {
        std::lock_guard lock(g_mutex);
        uint32_t start;
        const Range* range = FindRange(address, start);
        return range != nullptr ? g_names[range->name] : std::string();
    }
}

// The game's copy functions, all (destination, source, size): the package loader copies
// unpacked data into place with them. 0x8308F720 is the CRT's memcpy, 0x830B3988 another
// memcpy, 0x830B3900 XMemCpy.
#define NB_COPY_HOOK(address) \
    extern "C" PPC_FUNC(__imp__sub_##address); \
    PPC_FUNC(sub_##address) \
    { \
        uint32_t destination = ctx.r3.u32, source = ctx.r4.u32, size = ctx.r5.u32; \
        __imp__sub_##address(ctx, base); \
        origins::NoteCopy(source, destination, size); \
    }

NB_COPY_HOOK(8308F720)
NB_COPY_HOOK(830B3988)
NB_COPY_HOOK(830B3900)

// XMemDecompress(context, pDestination, pDestSize, pSource, SrcSize), statically linked: the
// game unpacks its data packages with it.
extern "C" PPC_FUNC(__imp__sub_830B5458);
PPC_FUNC(sub_830B5458)
{
    uint32_t destination = ctx.r4.u32;
    uint32_t destinationSize = ctx.r5.u32;
    uint32_t source = ctx.r6.u32;
    __imp__sub_830B5458(ctx, base);
    if (int32_t(ctx.r3.u32) >= 0 && destinationSize != 0)
        origins::NoteDecompress(source, destination, PPC_LOAD_U32(destinationSize));
}

#endif
