// Reporting for the generated stubs of imports that have no implementation yet.
#include "import_stub.h"

#include <cstdio>
#include <mutex>
#include <set>
#include <string>

void nb::ReportStub(const char* name, const PPCContext& ctx)
{
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::lock_guard lock(mutex);
    if (seen.insert(name).second)
    {
        fprintf(stderr, "[stub] %-40s r3=%08X r4=%08X r5=%08X r6=%08X lr=%08X\n", name,
            ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));
    }
}
