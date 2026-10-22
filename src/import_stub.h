#pragma once

#include <ppc_recomp_shared.h>

namespace nb
{
    // Logs the first call to an unimplemented import, then returns 0 (STATUS_SUCCESS).
    void ReportStub(const char* name, const PPCContext& ctx);
}

#define NB_IMPORT_STUB(name) \
    PPC_FUNC(__imp__##name) \
    { \
        nb::ReportStub(#name, ctx); \
        ctx.r3.u64 = 0; \
    }
