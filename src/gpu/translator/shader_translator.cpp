// Built with XenosRecomp's sources and precompiled header (see CMakeLists.txt).
#include "shader_translator.h"
#include "shader.h"
#include "shader_recompiler.h"
#include "dxc_compiler.h"
#include <shader_common_text.h>

namespace shader_translator
{
    static thread_local bool g_traceSteps = false;

    void TraceSteps(bool enabled)
    {
        g_traceSteps = enabled;
    }

    static void Step(const char* step, size_t value = 0)
    {
        if (g_traceSteps)
            fprintf(stderr, "[translator] %s (%zu)\n", step, value);
    }

    // Bump when the generated code changes for reasons other than the patches or the header.
    constexpr uint64_t TRANSLATOR_REVISION = 1;

    size_t ContainerSize(const uint8_t* data, size_t available)
    {
        if (available < sizeof(ShaderContainer))
            return 0;
        auto container = reinterpret_cast<const ShaderContainer*>(data);
        size_t size = size_t(container->virtualSize) + container->physicalSize;
        if ((container->flags & 0xFFFFFF00) != 0x102A1100 || container->field1C != 0 || container->field20 != 0 ||
            container->constantTableOffset == 0 || container->shaderOffset == 0 || size <= sizeof(ShaderContainer) ||
            size > available || size > 0x100000)
        {
            return 0;
        }
        return size;
    }

    bool Translate(const uint8_t* container, std::vector<uint8_t>& spirv, uint32_t& specConstantsMask, std::string& error)
    {
        // DxcCompiler only asserts that DXC started (nothing, in a release build), then uses
        // it: check once that it can.
        static const HRESULT available = []
            {
                IDxcCompiler3* compiler = nullptr;
                HRESULT result = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
                if (compiler != nullptr)
                    compiler->Release();
                return compiler != nullptr ? result : E_FAIL;
            }();
        if (FAILED(available))
        {
            error = fmt::format("DXC could not start (0x{:08X})", uint32_t(available));
            return false;
        }
        Step("DXC available");

        ShaderRecompiler recompiler;
        recompiler.recompile(container, std::string_view(SHADER_COMMON_TEXT));
        specConstantsMask = recompiler.specConstantsMask;
        Step("HLSL written, characters", recompiler.out.size());

        thread_local DxcCompiler compiler;
        Step("compiler ready; compiling");
        IDxcBlob* blob = compiler.compile(recompiler.out, recompiler.isPixelShader, false, true);
        Step("compiled", blob != nullptr ? blob->GetBufferSize() : 0);
        if (blob == nullptr)
        {
            error = "DXC could not compile the translated shader";
            return false;
        }
        auto* begin = static_cast<const uint8_t*>(blob->GetBufferPointer());
        spirv.assign(begin, begin + blob->GetBufferSize());
        blob->Release();
        return true;
    }

    uint64_t Version()
    {
        return XXH3_64bits(SHADER_COMMON_TEXT, sizeof(SHADER_COMMON_TEXT) - 1)
            ^ XXH3_64bits(TRANSLATOR_PATCH_HASH, sizeof(TRANSLATOR_PATCH_HASH) - 1) ^ TRANSLATOR_REVISION;
    }
}
