// Precompiled header for the runtime. Adapted from Unleashed Recompiled's stdafx.h
// with the graphics, audio and UI dependencies removed.
#pragma once

#define NOMINMAX

#if defined(__linux__) || defined(__PROSPERO__)
#include <unistd.h>
#include <pwd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <numeric>
#include <optional>
#include <semaphore>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <xbox.h>
#include <xxhash.h>
#include <ankerl/unordered_dense.h>
#include <ppc_recomp_shared.h>
#include <o1heap.h>
#include <fmt/core.h>

#include "framework.h"

#if defined(__PROSPERO__) && !defined(__cpp_lib_atomic_ref)
// The PS5 SDK's libc++ (18) has no std::atomic_ref yet: the operations the runtime uses, on
// the compiler's atomic builtins. wait() polls; notify does nothing.
namespace std
{
    template<typename T>
    struct atomic_ref
    {
        T* ptr;

        explicit atomic_ref(T& value) : ptr(&value) {}

        T load(memory_order order = memory_order_seq_cst) const { return __atomic_load_n(ptr, int(order)); }
        void store(T value, memory_order order = memory_order_seq_cst) const { __atomic_store_n(ptr, value, int(order)); }
        T operator=(T value) const { store(value); return value; }
        operator T() const { return load(); }
        T exchange(T value, memory_order order = memory_order_seq_cst) const { return __atomic_exchange_n(ptr, value, int(order)); }
        bool compare_exchange_weak(T& expected, T desired, memory_order order = memory_order_seq_cst) const
        {
            return __atomic_compare_exchange_n(ptr, &expected, desired, true, int(order), __ATOMIC_SEQ_CST);
        }
        bool compare_exchange_strong(T& expected, T desired, memory_order order = memory_order_seq_cst) const
        {
            return __atomic_compare_exchange_n(ptr, &expected, desired, false, int(order), __ATOMIC_SEQ_CST);
        }
        T fetch_add(T value, memory_order order = memory_order_seq_cst) const { return __atomic_fetch_add(ptr, value, int(order)); }
        T fetch_sub(T value, memory_order order = memory_order_seq_cst) const { return __atomic_fetch_sub(ptr, value, int(order)); }
        T fetch_or(T value, memory_order order = memory_order_seq_cst) const { return __atomic_fetch_or(ptr, value, int(order)); }
        T fetch_and(T value, memory_order order = memory_order_seq_cst) const { return __atomic_fetch_and(ptr, value, int(order)); }

        void wait(T old, memory_order order = memory_order_seq_cst) const
        {
            for (uint32_t spins = 0; load(order) == old; spins++)
            {
                if (spins < 64)
                    __builtin_ia32_pause();
                else
                    this_thread::yield();
            }
        }
        void notify_one() const {}
        void notify_all() const {}
    };
}
#endif
#include "mutex.h"

#ifndef _WIN32
#include <sys/mman.h>
#endif
