// Print a disassembly listing: disas [XEX] [address] [before=24] [after=2]
#include <file.h>
#include <disasm.h>
#include <image.h>
#include <fmt/core.h>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fmt::println("Usage: disas [XEX] [address] [before=24] [after=2]");
        return 1;
    }
    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());
    uint32_t addr = std::stoul(argv[2], nullptr, 16);
    int before = argc > 3 ? std::stoi(argv[3]) : 24;
    int after = argc > 4 ? std::stoi(argv[4]) : 2;
    ppc_insn insn;
    for (int i = -before; i <= after; i++)
    {
        uint32_t a = addr + i * 4;
        auto* p = (const uint32_t*)image.Find(a);
        if (!p) continue;
        ppc::Disassemble(p, a, insn);
        fmt::println("{}{:08X}  {:<10} {}", i == 0 ? '>' : ' ', a, insn.opcode ? insn.opcode->name : "???", insn.op_str);
    }
}
