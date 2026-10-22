// Find where PM4 type-3 packet headers for given opcodes are built anywhere in the code:
// lis rX, 0xC0nn followed (within 6 instructions) by ori/addi/oris forming opcode << 8.
// pm4scan [XEX] [opcode hex...]
#include <file.h>
#include <image.h>
#include <xbox.h>
#include <fmt/core.h>
#include <map>
#include <set>

int main(int argc, char** argv)
{
    if (argc < 3)
        return 1;
    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());
    std::set<uint32_t> wanted;
    for (int i = 2; i < argc; i++)
        wanted.insert(std::stoul(argv[i], nullptr, 16));

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

    for (const auto& section : image.sections)
    {
        if (!(section.flags & SectionFlags_Code))
            continue;
        auto* code = (const uint32_t*)section.data;
        size_t n = section.size / 4;
        for (size_t i = 0; i < n; i++)
        {
            uint32_t insn = ByteSwap(code[i]);
            if ((insn >> 26) != 15 || ((insn >> 16) & 0x1F) != 0)
                continue;
            uint32_t hi = insn & 0xFFFF;
            if (hi < 0xC000 || hi > 0xC0FF)
                continue;
            uint32_t rd = (insn >> 21) & 0x1F;
            for (size_t k = 1; k <= 6 && i + k < n; k++)
            {
                uint32_t next = ByteSwap(code[i + k]);
                uint32_t op = next >> 26;
                if ((op == 24 || op == 14) && ((next >> 16) & 0x1F) == rd && (next & 0xFF) == 0)
                {
                    uint32_t opcode = (next >> 8) & 0xFF;
                    if (wanted.count(opcode))
                    {
                        uint32_t addr = uint32_t(section.base + i * 4);
                        fmt::println("{:08X} in {:08X}: opcode {:02X} count {}", addr, containing(addr), opcode, hi - 0xC000 + 1);
                    }
                    break;
                }
            }
        }
    }
}
