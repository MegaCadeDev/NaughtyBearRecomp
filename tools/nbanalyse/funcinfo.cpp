// Report which .pdata function (if any) covers each address: funcinfo [XEX] [addr...]
#include <file.h>
#include <image.h>
#include <xbox.h>
#include <fmt/core.h>
#include <map>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 3)
        return 1;
    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());

    std::map<uint32_t, uint32_t> pdata;
    if (const auto* section = image.Find(".pdata"))
    {
        auto* pf = (const IMAGE_CE_RUNTIME_FUNCTION*)section->data;
        for (size_t i = 0; i < section->size / sizeof(IMAGE_CE_RUNTIME_FUNCTION); i++)
        {
            IMAGE_CE_RUNTIME_FUNCTION fn = pf[i];
            fn.BeginAddress = ByteSwap(fn.BeginAddress);
            fn.Data = ByteSwap(fn.Data);
            pdata[fn.BeginAddress] = fn.FunctionLength * 4;
        }
    }

    for (int i = 2; i < argc; i++)
    {
        uint32_t addr = std::stoul(argv[i], nullptr, 16);
        auto it = pdata.upper_bound(addr);
        if (it != pdata.begin() && addr < std::prev(it)->first + std::prev(it)->second)
            fmt::println("0x{:X}: in pdata 0x{:X} size 0x{:X}", addr, std::prev(it)->first, std::prev(it)->second);
        else
            fmt::println("0x{:X}: no pdata (previous 0x{:X}+0x{:X}, next 0x{:X})", addr,
                it != pdata.begin() ? std::prev(it)->first : 0, it != pdata.begin() ? std::prev(it)->second : 0,
                it != pdata.end() ? it->first : 0);
    }
}
