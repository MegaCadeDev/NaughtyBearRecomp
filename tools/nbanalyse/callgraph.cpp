// Survey the functions in an address range: who calls them from outside the range, what
// they call, which imports they use, and which PM4 packet headers they build. Used to map
// the statically linked Direct3D library.
//
// callgraph [XEX] [range begin] [range end]
#include <file.h>
#include <disasm.h>
#include <image.h>
#include <xbox.h>
#include <fmt/core.h>
#include <map>
#include <set>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        fmt::println("Usage: callgraph [XEX] [begin] [end]");
        return 1;
    }
    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());
    uint32_t begin = std::stoul(argv[2], nullptr, 16);
    uint32_t end = std::stoul(argv[3], nullptr, 16);

    // Function starts and sizes from .pdata.
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

    std::map<uint32_t, std::string> imports;
    for (const auto& sym : image.symbols)
        if (sym.name.starts_with("__imp__"))
            imports[uint32_t(sym.address)] = sym.name.substr(7);

    auto containing = [&](uint32_t addr) -> uint32_t
        {
            auto it = functions.upper_bound(addr);
            if (it == functions.begin())
                return 0;
            --it;
            return addr < it->first + it->second ? it->first : 0;
        };

    struct Info
    {
        std::set<uint32_t> externalCallers;
        std::set<uint32_t> internalCallers;
        std::set<std::string> importsUsed;
        std::set<uint32_t> callees;
        std::set<uint32_t> pm4;
    };
    std::map<uint32_t, Info> info;
    // Branch targets inside the range that are not .pdata starts still count as functions.
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
            if (op == 18 && (insn & 1)) // bl
            {
                uint32_t target = addr + uint32_t((int32_t(insn << 6) >> 6) & ~3);
                uint32_t caller = containing(addr);
                bool callerIn = addr >= begin && addr < end;
                if (target >= begin && target < end)
                {
                    auto& t = info[target];
                    (callerIn ? t.internalCallers : t.externalCallers).insert(caller ? caller : addr);
                }
                if (callerIn && caller)
                {
                    if (auto imp = imports.find(target); imp != imports.end())
                        info[caller].importsUsed.insert(imp->second);
                    else
                        info[caller].callees.insert(target);
                }
            }
            // lis rX, 0xC0xx: the high half of a PM4 type-3 packet header (count | opcode).
            // Require the full header: lis rX, 0xC0nn then ori rX, rX, opcode << 8 nearby.
            if (op == 15 && ((insn >> 16) & 0x1F) == 0 && addr >= begin && addr < end)
            {
                uint32_t hi = insn & 0xFFFF;
                uint32_t rd = (insn >> 21) & 0x1F;
                if (hi >= 0xC000 && hi <= 0xC0FF)
                {
                    for (size_t k = 1; k <= 4 && i + k < section.size / 4; k++)
                    {
                        uint32_t next = ByteSwap(code[i + k]);
                        if ((next >> 26) == 24 && ((next >> 21) & 0x1F) == rd && ((next >> 16) & 0x1F) == rd && (next & 0xFF) == 0)
                        {
                            if (uint32_t caller = containing(addr))
                                info[caller].pm4.insert((hi << 16) | (next & 0xFFFF));
                            break;
                        }
                    }
                }
            }
        }
    }

    for (auto& [addr, size] : functions)
    {
        if (addr < begin || addr >= end)
            continue;
        auto& i = info[addr];
        std::string imps, pm4;
        for (auto& s : i.importsUsed)
            imps += s + " ";
        for (auto v : i.pm4)
            pm4 += fmt::format("{:02X}/{} ", (v >> 8) & 0xFF, ((v >> 16) & 0x3FFF) + 1);
        fmt::println("{:08X} size {:5X} ext {:3} int {:3} callees {:3} {}{}", addr, size, i.externalCallers.size(), i.internalCallers.size(),
            i.callees.size(), imps.empty() ? "" : "imports: " + imps, pm4.empty() ? "" : "pm4: " + pm4);
    }
}
