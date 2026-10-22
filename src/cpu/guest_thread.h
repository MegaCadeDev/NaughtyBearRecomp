#pragma once

#include <kernel/xdm.h>
#include <deque>
#include <functional>
#include <mutex>

// Use pthreads directly on macOS to be able to increase default stack size.
#ifdef __APPLE__
#define USE_PTHREAD 1
#endif

#ifdef USE_PTHREAD
#include <pthread.h>
#endif

#define CURRENT_THREAD_HANDLE uint32_t(-2)

struct GuestThreadContext
{
    PPCContext ppcContext{};
    uint8_t* thread = nullptr;

    GuestThreadContext(uint32_t cpuNumber);
    ~GuestThreadContext();
};

struct GuestThreadParams
{
    uint32_t function;
    uint32_t value;
    uint32_t flags;
    uint32_t value2 = 0; // second argument (r4), used for the XAPI thread startup routine
};

// A user-mode APC queued by NtQueueApcThread: routine(context, arg1, arg2) runs on the
// target thread the next time it enters an alertable wait.
struct GuestApc
{
    uint32_t routine;
    uint32_t context;
    uint32_t arg1;
    uint32_t arg2;
};

struct GuestApcQueue
{
    std::mutex mutex;
    std::deque<GuestApc> apcs;
    std::atomic<bool> pending = false;
};

struct GuestThreadHandle : KernelObject
{
    GuestThreadParams params;
    GuestApcQueue apcQueue;
    std::atomic<bool> suspended;
    std::atomic<bool> finished = false;
#ifdef USE_PTHREAD
    pthread_t thread;
#else
    std::thread thread;
#endif

    GuestThreadHandle(const GuestThreadParams& params);
    ~GuestThreadHandle() override;

    uint32_t GetThreadId() const;

    uint32_t Wait(uint32_t timeout) override;
};

// Thrown by ExTerminateThread to unwind the guest thread back to GuestThread::Start.
struct GuestThreadExit
{
    uint32_t exitCode;
};

struct GuestThread
{
    static uint32_t Start(const GuestThreadParams& params);
    static GuestThreadHandle* Start(const GuestThreadParams& params, uint32_t* threadId);

    static uint32_t GetCurrentThreadId();

    static void QueueApc(GuestThreadHandle* hThread, const GuestApc& apc);
    // Runs the current thread's pending APCs; returns true if any ran.
    static bool DeliverApcs();
    // Performs an alertable wait: `wait` is called with time slices (in milliseconds) and
    // pending APCs are delivered between them. Returns STATUS_USER_APC if any APC ran.
    static uint32_t AlertableWait(uint32_t timeout, const std::function<uint32_t(uint32_t)>& wait);
    static void SetLastError(uint32_t error);

#ifdef _WIN32
    static void SetThreadName(uint32_t threadId, const char* name);
#endif
};
