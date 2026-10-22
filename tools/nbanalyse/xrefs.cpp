// List branches (bl/b/bc) to each address and the .pdata function they sit in: xrefs [XEX] [addr...]
#include <file.h>
#include <image.h>
#include <xbox.h>
#include <fmt/core.h>
#include <map>
#include <set>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 3)
        return 1;
    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());

    std::map<uint32_t, uint32_t> functions;
    if (const auto* pdata = image.Find(".pdata"))
    {
        auto* pf = (const IMAGE_CE_RUNTIME_FUNCTION*)pdata->data;
        for (size_t i = 0; i < pdata->size / sizeof(IMAGE_CE_RUNTIME_FUNCTION); i++)
        {
            IMAGE_CE_RUNTIME_FUNCTION fn = pf[i];
            fn.BeginAddress = ByteSwap(fn.BeginAddress);
            fn.Data = ByteSwap(fn.Data);
            functions[fn.BeginAddress] = fn.FunctionLength * 4;
        }
    }
    auto containing = [&](uint32_t addr) -> uint32_t
        {
            auto it = functions.upper_bound(addr);
            if (it == functions.begin())
                return 0;
            --it;
            return addr < it->first + it->second ? it->first : 0;
        };

    std::set<uint32_t> targets;
    for (int i = 2; i < argc; i++)
        targets.insert(std::stoul(argv[i], nullptr, 16));

    for (const auto& section : image.sections)
    {
        if (!(section.flags & SectionFlags_Code))
            continue;
        auto* code = (const uint32_t*)section.data;
        for (size_t i = 0; i < section.size / 4; i++)
        {
            uint32_t addr = uint32_t(section.base + i * 4);
            uint32_t insn = ByteSwap(code[i]);
            uint32_t op = insn >> 26;
            uint32_t target = 0;
            const char* kind = nullptr;
            if (op == 18 && !(insn & 2))
            {
                target = addr + uint32_t((int32_t(insn << 6) >> 6) & ~3);
                kind = (insn & 1) ? "bl" : "b";
            }
            else if (op == 16 && !(insn & 2))
            {
                target = addr + uint32_t(int32_t(int16_t(insn & 0xFFFC)));
                kind = (insn & 1) ? "bcl" : "bc";
            }
            if (kind && targets.count(target))
                fmt::println("{:08X} -> {:08X} {:3} in {:08X}", addr, target, kind, containing(addr));
        }
    }
}
