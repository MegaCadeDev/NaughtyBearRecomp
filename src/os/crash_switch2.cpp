// Switch 2: faults reach libnx's exception handler (the PC version uses signals). It logs
// where the fault happened, as an offset into the program (switch2/tools/symbolize.sh maps it
// to a function), shows the error screen and ends the process.
#if defined(__SWITCH2__)

#include <stdafx.h>
#include <kernel/memory.h>
#include <os/process.h>
#include <os/switch2.h>

extern "C"
{
    extern char __start__[]; // the program's first byte (libounce's linker script)

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
