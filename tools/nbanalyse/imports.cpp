// List imported functions and every bl call site to them: imports [XEX] [name filter]
#include <file.h>
#include <disasm.h>
#include <image.h>
#include <xbox.h>
#include <fmt/core.h>
#include <map>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 2)
        return 1;
    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());
    std::string filter = argc > 2 ? argv[2] : "";

    std::map<uint32_t, std::string> imports;
    for (const auto& sym : image.symbols)
        if (sym.type == Symbol_Function && sym.name.find(filter) != std::string::npos)
            imports[uint32_t(sym.address)] = sym.name;

    for (auto& [addr, name] : imports)
        fmt::println("import 0x{:X} {}", addr, name);
    if (filter.empty())
        return 0;

    for (const auto& section : image.sections)
    {
        if (!(section.flags & SectionFlags_Code))
            continue;
        auto* code = (const uint32_t*)section.data;
        ppc_insn insn;
        for (size_t i = 0; i < section.size / 4; i++)
        {
            ppc::Disassemble(&code[i], section.base + i * 4, insn);
            if (insn.opcode && (insn.opcode->id == PPC_INST_BL || insn.opcode->id == PPC_INST_B) && imports.count(insn.operands[0]))
                fmt::println("call 0x{:X} -> {}", section.base + i * 4, imports[insn.operands[0]]);
        }
    }
}
