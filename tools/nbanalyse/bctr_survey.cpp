// Survey every bctr in the image and histogram the instruction sequences
// leading up to it, so jump table patterns XenonAnalyse misses can be found.
#include <file.h>
#include <disasm.h>
#include <image.h>
#include <fmt/core.h>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fmt::println("Usage: bctr_survey [XEX] [window=8]");
        return 1;
    }
    int window = argc > 2 ? std::stoi(argv[2]) : 8;
    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());

    std::map<std::string, std::vector<size_t>> patterns;
    size_t total = 0;
    for (const auto& section : image.sections)
    {
        if (!(section.flags & SectionFlags_Code))
            continue;
        fmt::println("code section {} base 0x{:X} size 0x{:X}", section.name, section.base, section.size);
        auto* code = (const uint32_t*)section.data;
        size_t count = section.size / 4;
        ppc_insn insn;
        for (size_t i = 0; i < count; i++)
        {
            ppc::Disassemble(&code[i], section.base + i * 4, insn);
            if (insn.opcode == nullptr || insn.opcode->id != PPC_INST_BCTR)
                continue;
            total++;
            std::string key;
            for (int k = window; k >= 1; k--)
            {
                if (i < (size_t)k) continue;
                ppc::Disassemble(&code[i - k], section.base + (i - k) * 4, insn);
                key += insn.opcode ? insn.opcode->name : "???";
                key += ' ';
            }
            patterns[key].push_back(section.base + i * 4);
        }
    }

    std::vector<std::pair<std::string, std::vector<size_t>>> sorted(patterns.begin(), patterns.end());
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second.size() > b.second.size(); });
    fmt::println("total bctr: {}, distinct {}-insn prefixes: {}", total, window, sorted.size());
    for (size_t i = 0; i < sorted.size(); i++)
        {
            fmt::println("{:6}  {} (e.g. 0x{:X})", sorted[i].second.size(), sorted[i].first, sorted[i].second[0]);
            if (getenv("SURVEY_ALL"))
                for (auto a : sorted[i].second)
                    fmt::println("    0x{:X}", a);
        }
}
