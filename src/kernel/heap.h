#pragma once

#include "mutex.h"
#include <map>

struct Heap
{
    Mutex mutex;
    O1HeapInstance* heap;

    // Physical memory: exact-size first-fit ranges in 0xA0000000-0xC0000000, so every
    // allocation has a unique GPU (29-bit physical) address, as on the console's 512 MB.
    // Requests that don't fit fall back to an o1heap above it.
    Mutex physicalMutex;
    std::map<uint32_t, uint32_t> physicalFree;      // guest start -> size
    std::map<uint32_t, uint32_t> physicalAllocated; // guest start -> size
    O1HeapInstance* physicalOverflowHeap;

    void Init();

    void* Alloc(size_t size);
    void* AllocPhysical(size_t size, size_t alignment);
    void Free(void* ptr);

    size_t Size(void* ptr);

    template<typename T, typename... Args>
    T* Alloc(Args&&... args)
    {
        T* obj = (T*)Alloc(sizeof(T));
        new (obj) T(std::forward<Args>(args)...);
        return obj;
    }

    template<typename T, typename... Args>
    T* AllocPhysical(Args&&... args)
    {
        T* obj = (T*)AllocPhysical(sizeof(T), alignof(T));
        new (obj) T(std::forward<Args>(args)...);
        return obj;
    }
};

extern Heap g_userHeap;
