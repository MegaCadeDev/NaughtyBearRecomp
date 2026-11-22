// Switch 2: faults reach libnx's exception handler (the PC version uses signals). It logs
// where the fault happened, as an offset into the program (switch2/tools/symbolize.sh maps it
// to a function), shows the error screen and ends the process. abort() and failed assertions
// do the same: newlib calls the __syscall_ hooks here, which libounce doesn't provide (without
// them, abort() returns to hbmenu with the game's threads still running).
//
// The console output (file descriptors 1 and 2: stdout, stderr, and LLVM's and the driver's
// messages, which write to the descriptors directly) goes to the log, through libnx's device
// table.
#if defined(__SWITCH2__)

#include <stdafx.h>
#include <kernel/memory.h>
#include <os/process.h>
#include <os/switch2.h>
#include <fcntl.h>
#include <sys/iosupport.h>
#include <unwind.h>

extern "C"
{
    extern char __start__[]; // the program's first byte (libounce's linker script)
}

// The calls that led here, as offsets into the program (from .eh_frame, with libgcc's
// unwinder): the terminate handler logs where an unhandled exception was thrown.
void LogBacktrace()
{
    struct State
    {
        uintptr_t program;
        int depth;
    } state{ reinterpret_cast<uintptr_t>(__start__), 0 };
    fprintf(stderr, "[nb] backtrace:");
    _Unwind_Backtrace([](_Unwind_Context* context, void* argument) -> _Unwind_Reason_Code
        {
            auto* state = static_cast<State*>(argument);
            uintptr_t ip = _Unwind_GetIP(context);
            if (ip == 0 || state->depth++ >= 32)
                return _URC_END_OF_STACK;
            fprintf(stderr, " NaughtyBear.elf+0x%llX", (unsigned long long)(ip - state->program));
            return _URC_NO_REASON;
        }, &state);
    fprintf(stderr, "\n");
}

namespace
{
    int g_logFile = -1;
    std::mutex g_logMutex;

    ssize_t WriteLog(struct _reent*, void*, const char* data, size_t length)
    {
        std::lock_guard lock(g_logMutex);
        for (size_t written = 0; written < length;)
        {
            ssize_t result = write(g_logFile, data + written, length - written);
            if (result <= 0)
                break;
            written += size_t(result);
        }
        return ssize_t(length);
    }

    const devoptab_t g_logDevice = { .name = "log", .structSize = 0, .write_r = WriteLog };
}

void RedirectConsoleToLog(const char* path)
{
    g_logFile = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_logFile < 0)
        return;
    devoptab_list[STD_OUT] = &g_logDevice;
    devoptab_list[STD_ERR] = &g_logDevice;
}

[[noreturn]] static void Abort(const char* reason)
{
    fprintf(stderr, "\n[nb] %s\n", reason);
    LogBacktrace();
    os::process::ShowMessage(std::string("Naughty Bear Recompiled stopped (") + reason + "). See log.txt in its folder.");
    os::process::Exit(EXIT_FAILURE);
}

extern "C"
{
    void __syscall_abort(void)
    {
        Abort("abort() was called");
    }

    void __syscall_assert_func(const char* file, int line, const char* function, const char* expression)
    {
        char reason[512];
        snprintf(reason, sizeof(reason), "assertion failed: %s, in %s at %s:%d", expression, function != nullptr ? function : "?", file, line);
        Abort(reason);
    }

    // libnx runs the handler on this stack, so it works when the faulting thread's is spent.
    alignas(16) uint8_t __nx_exception_stack[0x8000];
    uint64_t __nx_exception_stack_size = sizeof(__nx_exception_stack);

    void __libnx_exception_handler(ThreadExceptionDump* context)
    {
        auto program = reinterpret_cast<uintptr_t>(__start__);
        auto base = reinterpret_cast<uintptr_t>(g_memory.base);
        uintptr_t pc = context->pc.x;
        uintptr_t address = context->far.x;
        char where[96];
        if (base != 0 && address >= base && address < base + PPC_MEMORY_SIZE)
            snprintf(where, sizeof(where), "guest memory access at 0x%08llX", (unsigned long long)(address - base));
        else
            snprintf(where, sizeof(where), "address 0x%016llX", (unsigned long long)address);
        char message[256];
        snprintf(message, sizeof(message), "exception 0x%X at NaughtyBear.elf+0x%llX, %s", context->error_desc,
            (unsigned long long)(pc - program), where);
        fprintf(stderr, "\n[nb] %s\n[nb] lr NaughtyBear.elf+0x%llX, sp 0x%016llX\n", message, (unsigned long long)(context->lr.x - program),
            (unsigned long long)context->sp.x);
        os::process::ShowMessage(std::string("Naughty Bear Recompiled crashed (") + message + "). See log.txt in its folder.");
        os::process::Exit(EXIT_FAILURE);
    }
}

#endif
