// Histogram the exception handler words stored in the 8 bytes before each .pdata
// function flagged with an exception handler. These need invalid_instructions entries.
#include <file.h>
#include <image.h>
#include <xbox.h>
#include <fmt/core.h>
#include <map>

int main(int argc, char** argv)
{
    if (argc < 2)
        return 1;
    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());
    const auto* section = image.Find(".pdata");
    auto* pf = (const IMAGE_CE_RUNTIME_FUNCTION*)section->data;
    std::map<uint32_t, size_t> handlers;
    size_t flagged = 0, total = section->size / sizeof(IMAGE_CE_RUNTIME_FUNCTION);
    for (size_t i = 0; i < total; i++)
    {
        IMAGE_CE_RUNTIME_FUNCTION fn = pf[i];
        fn.BeginAddress = ByteSwap(fn.BeginAddress);
        fn.Data = ByteSwap(fn.Data);
        if (!fn.ExceptionFlag)
            continue;
        flagged++;
        auto* p = (const uint32_t*)image.Find(fn.BeginAddress - 8);
        if (p)
            handlers[ByteSwap(p[0])]++;
    }
    fmt::println("pdata entries: {}, with exception handler: {}", total, flagged);
    for (auto& [h, n] : handlers)
        fmt::println("handler 0x{:08X}: {}", h, n);
}
