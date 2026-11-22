#include <stdafx.h>
#include "guest_thread.h"
#include <kernel/memory.h>
#include <kernel/heap.h>
#include <kernel/function.h>
#include "ppc_context.h"
#if defined(__SWITCH2__)
#include <os/switch2.h>
#endif

constexpr size_t PCR_SIZE = 0xAB0;
constexpr size_t TLS_SIZE = 0x100;
constexpr size_t TEB_SIZE = 0x2E0;
constexpr size_t STACK_SIZE = 0x40000;
constexpr size_t TOTAL_SIZE = PCR_SIZE + TLS_SIZE + TEB_SIZE + STACK_SIZE;

constexpr size_t TEB_OFFSET = PCR_SIZE + TLS_SIZE;

GuestThreadContext::GuestThreadContext(uint32_t cpuNumber)
{
    assert(thread == nullptr);

    thread = (uint8_t*)g_userHeap.Alloc(TOTAL_SIZE);
    memset(thread, 0, TOTAL_SIZE);

    *(uint32_t*)thread = ByteSwap(g_memory.MapVirtual(thread + PCR_SIZE)); // tls pointer
    *(uint32_t*)(thread + 0x100) = ByteSwap(g_memory.MapVirtual(thread + PCR_SIZE + TLS_SIZE)); // teb pointer
    *(thread + 0x10C) = cpuNumber;

    *(uint32_t*)(thread + PCR_SIZE + 0x10) = 0xFFFFFFFF; // that one TLS entry that felt quirky
    *(uint32_t*)(thread + PCR_SIZE + TLS_SIZE + 0x14C) = ByteSwap(GuestThread::GetCurrentThreadId()); // thread id

    ppcContext.r1.u64 = g_memory.MapVirtual(thread + PCR_SIZE + TLS_SIZE + TEB_SIZE + STACK_SIZE); // stack pointer
    ppcContext.r13.u64 = g_memory.MapVirtual(thread);
    ppcContext.fpscr.loadFromHost();

    assert(GetPPCContext() == nullptr);
    SetPPCContext(ppcContext);
}

GuestThreadContext::~GuestThreadContext()
{
    g_userHeap.Free(thread);
}

// The APC queue of the current guest thread (threads not created through a handle, like the
// main thread, get one that nothing can queue to).
static thread_local GuestApcQueue* g_currentApcQueue;

#if defined(__SWITCH2__)
// The cores the process may use (a kaiHEN program gets six).
static uint64_t GetCoreMask()
{
    static uint64_t mask = []
        {
            uint64_t value = 0;
            if (R_FAILED(svcGetInfo(&value, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || value == 0)
                value = 1;
            return value;
        }();
    return mask;
}

// libnx starts every thread on the process's default core. Each one gets the next core in
// turn as its preferred core, and may run on any of them.
static void PlaceOnCore()
{
    static std::atomic<uint32_t> next = 0;
    uint64_t mask = GetCoreMask();
    uint32_t index = next++ % uint32_t(std::popcount(mask));
    int32_t core = 0;
    for (uint64_t remaining = mask; remaining != 0; remaining &= remaining - 1, index--)
    {
        if (index == 0)
        {
            core = std::countr_zero(remaining);
            break;
        }
    }
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, uint32_t(mask));
}
#endif

uint32_t GetHostCoreCount()
{
#if defined(__SWITCH2__)
    return uint32_t(std::popcount(GetCoreMask()));
#else
    return std::max(1u, std::thread::hardware_concurrency());
#endif
}

#ifdef USE_PTHREAD
static size_t GetStackSize()
{
    // Cache as this should not change.
    static size_t stackSize = 0;
    if (stackSize == 0)
    {
        // 8 MiB is a typical default.
        constexpr auto defaultSize = 8 * 1024 * 1024;
#if defined(__PROSPERO__) || defined(__SWITCH2__)
        // The consoles' defaults are far smaller than recompiled code needs, and they have no
        // limit to ask.
        stackSize = defaultSize;
#else
        struct rlimit lim;
        const auto ret = getrlimit(RLIMIT_STACK, &lim);
        if (ret == 0 && lim.rlim_cur < defaultSize)
        {
            // Use what the system allows.
            stackSize = lim.rlim_cur;
        }
        else
        {
            stackSize = defaultSize;
        }
#endif
    }
    return stackSize;
}

static void* GuestThreadFunc(void* arg)
{
    GuestThreadHandle* hThread = (GuestThreadHandle*)arg;
#else
static void GuestThreadFunc(GuestThreadHandle* hThread)
{
#endif
#if defined(__SWITCH2__)
    PlaceOnCore();
#endif
    hThread->suspended.wait(true);
    g_currentApcQueue = &hThread->apcQueue;
    GuestThread::Start(hThread->params);
    hThread->finished = true;
    hThread->finished.notify_all();
#ifdef USE_PTHREAD
    return nullptr;
#endif
}

GuestThreadHandle::GuestThreadHandle(const GuestThreadParams& params)
    : params(params), suspended((params.flags & 0x1) != 0)
#ifdef USE_PTHREAD
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, GetStackSize());
    const auto ret = pthread_create(&thread, &attr, GuestThreadFunc, this);
    if (ret != 0) {
        fprintf(stderr, "pthread_create failed with error code 0x%X.\n", ret);
        return;
    }
}
#else
      , thread(GuestThreadFunc, this)
{
}
#endif

void StartHostThread(std::function<void()> function)
{
#ifdef USE_PTHREAD
    auto* start = new std::function<void()>(std::move(function));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, GetStackSize());
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const auto ret = pthread_create(&thread, &attr, [](void* argument) -> void*
        {
            std::unique_ptr<std::function<void()>> function((std::function<void()>*)argument);
#if defined(__SWITCH2__)
            PlaceOnCore();
#endif
            (*function)();
            return nullptr;
        }, start);
    pthread_attr_destroy(&attr);
    if (ret != 0)
    {
        fprintf(stderr, "pthread_create failed with error code 0x%X.\n", ret);
        delete start;
    }
#else
    std::thread(std::move(function)).detach();
#endif
}

GuestThreadHandle::~GuestThreadHandle()
{
#ifdef USE_PTHREAD
    pthread_join(thread, nullptr);
#else
    if (thread.joinable())
        thread.join();
#endif
}

template <typename ThreadType>
static uint32_t CalcThreadId(const ThreadType& id)
{
    if constexpr (sizeof(id) == 4)
        return *reinterpret_cast<const uint32_t*>(&id);
    else
        return XXH32(&id, sizeof(id), 0);
}

uint32_t GuestThreadHandle::GetThreadId() const
{
#ifdef USE_PTHREAD
    return CalcThreadId(thread);
#else
    return CalcThreadId(thread.get_id());
#endif
}

uint32_t GuestThreadHandle::Wait(uint32_t timeout)
{
    // Polls and timed waits check the finished flag; only an infinite wait joins.
    if (timeout != INFINITE)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
        while (!finished)
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return STATUS_TIMEOUT;
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }

#ifdef USE_PTHREAD
    pthread_join(thread, nullptr);
#else
    if (thread.joinable())
        thread.join();
#endif

    return STATUS_WAIT_0;
}

uint32_t GuestThread::Start(const GuestThreadParams& params)
{
    const auto procMask = (uint8_t)(params.flags >> 24);
    const auto cpuNumber = procMask == 0 ? 0 : 7 - std::countl_zero(procMask);

    GuestThreadContext ctx(cpuNumber);
    ctx.ppcContext.r3.u64 = params.value;
    ctx.ppcContext.r4.u64 = params.value2;

    try
    {
        g_memory.FindFunction(params.function)(ctx.ppcContext, g_memory.base);
    }
    catch (const GuestThreadExit& exit)
    {
        return exit.exitCode;
    }

    return ctx.ppcContext.r3.u32;
}

GuestThreadHandle* GuestThread::Start(const GuestThreadParams& params, uint32_t* threadId)
{
    auto hThread = CreateKernelObject<GuestThreadHandle>(params);

    if (threadId != nullptr)
    {
        *threadId = hThread->GetThreadId();
    }

    return hThread;
}

void GuestThread::QueueApc(GuestThreadHandle* hThread, const GuestApc& apc)
{
    std::lock_guard lock(hThread->apcQueue.mutex);
    hThread->apcQueue.apcs.push_back(apc);
    hThread->apcQueue.pending = true;
}

bool GuestThread::DeliverApcs()
{
    auto* queue = g_currentApcQueue;
    if (queue == nullptr || !queue->pending)
        return false;

    bool delivered = false;
    while (true)
    {
        GuestApc apc;
        {
            std::lock_guard lock(queue->mutex);
            if (queue->apcs.empty())
            {
                queue->pending = false;
                break;
            }
            apc = queue->apcs.front();
            queue->apcs.pop_front();
        }

        GuestToHostFunction<void>(apc.routine, apc.context, apc.arg1, apc.arg2);
        delivered = true;
    }

    return delivered;
}

uint32_t GuestThread::AlertableWait(uint32_t timeout, const std::function<uint32_t(uint32_t)>& wait)
{
    constexpr uint32_t SLICE_MS = 2;

    if (DeliverApcs())
        return STATUS_USER_APC;

    if (g_currentApcQueue == nullptr || timeout == 0)
        return wait(timeout);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    while (true)
    {
        uint32_t slice = SLICE_MS;
        if (timeout != INFINITE)
        {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            slice = uint32_t(std::clamp<int64_t>(remaining, 0, SLICE_MS));
        }

        uint32_t status = wait(slice);
        if (status != STATUS_TIMEOUT)
            return status;

        if (DeliverApcs())
            return STATUS_USER_APC;

        if (timeout != INFINITE && std::chrono::steady_clock::now() >= deadline)
            return STATUS_TIMEOUT;
    }
}

uint32_t GuestThread::GetCurrentThreadId()
{
#ifdef USE_PTHREAD
    return CalcThreadId(pthread_self());
#else
    return CalcThreadId(std::this_thread::get_id());
#endif
}

void GuestThread::SetLastError(uint32_t error)
{
    auto* thread = (char*)g_memory.Translate(GetPPCContext()->r13.u32);
    if (*(uint32_t*)(thread + 0x150))
    {
        // Program doesn't want errors
        return;
    }

    // TEB + 0x160 : Win32LastError
    *(uint32_t*)(thread + TEB_OFFSET + 0x160) = ByteSwap(error);
}

#ifdef _WIN32
void GuestThread::SetThreadName(uint32_t threadId, const char* name)
{
#pragma pack(push,8)
    const DWORD MS_VC_EXCEPTION = 0x406D1388;

    typedef struct tagTHREADNAME_INFO
    {
        DWORD dwType; // Must be 0x1000.
        LPCSTR szName; // Pointer to name (in user addr space).
        DWORD dwThreadID; // Thread ID (-1=caller thread).
        DWORD dwFlags; // Reserved for future use, must be zero.
    } THREADNAME_INFO;
#pragma pack(pop)

    THREADNAME_INFO info;
    info.dwType = 0x1000;
    info.szName = name;
    info.dwThreadID = threadId;
    info.dwFlags = 0;

    __try
    {
        RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}
#endif

void SetThreadNameImpl(uint32_t a1, uint32_t threadId, uint32_t* name)
{
#ifdef _WIN32
    GuestThread::SetThreadName(threadId, (const char*)g_memory.Translate(ByteSwap(*name)));
#endif
}

int GetThreadPriorityImpl(GuestThreadHandle* hThread)
{
#ifdef _WIN32
    return GetThreadPriority(hThread == GetKernelObject(CURRENT_THREAD_HANDLE) ? GetCurrentThread() : hThread->thread.native_handle());
#else 
    return 0;
#endif
}

uint32_t SetThreadIdealProcessorImpl(GuestThreadHandle* hThread, uint32_t dwIdealProcessor)
{
    return 0;
}
