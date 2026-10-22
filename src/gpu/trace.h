// Runtime tracing of calls into the Direct3D library (see tools/gen_trace_wrappers.py).
#pragma once

#include <ppc_recomp_shared.h>

namespace nb::trace
{
    // Range treated as the Direct3D library; calls from outside it are API calls.
    constexpr uint32_t D3D_BEGIN = 0x82F1C000;
    constexpr uint32_t D3D_END = 0x82F50000;

    void Enter(uint32_t function, const PPCContext& ctx);
    void Leave(uint32_t function, const PPCContext& ctx);

    // Writes the call counts gathered so far to the log.
    void Report();
}
