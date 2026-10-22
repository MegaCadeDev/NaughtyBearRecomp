// Jump table detector for Naughty Bear.
//
// XenonAnalyse matches fixed instruction sequences tuned for Sonic Unleashed.
// Naughty Bear's compiler reorders those sequences and pads them with nops,
// so instead of matching opcodes this walks back from every bctr to the
// bounds check (cmplwi + bgt/ble) and symbolically executes the instructions
// in between to recover the table address, entry size and label base.
//
// Output uses the same [[switch]] TOML format that XenonRecomp consumes.

#include <file.h>
#include <disasm.h>
#include <image.h>
#include <xbox.h>
#include <fmt/core.h>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace
{
    // What a register is known to hold while executing the dispatch sequence.
    enum class Kind
    {
        Unknown,
        Const,   // value
        Index,   // switch index << shift
        Entry,   // table entry loaded from table base `value`, then << shift
        Target,  // value + (table entry << shift)
    };

    struct Value
    {
        Kind kind = Kind::Unknown;
        uint32_t value = 0;
        uint32_t shift = 0;
        uint32_t entrySize = 0;
        uint32_t tableAddr = 0;
    };

    struct SwitchTable
    {
        uint32_t base = 0;
        uint32_t bctr = 0;
        uint32_t r = 0;
        uint32_t defaultLabel = 0;
        uint32_t entrySize = 0;
        std::vector<uint32_t> labels;
    };

    bool IsRotateLeftMask(const ppc_insn& insn, uint32_t& shift)
    {
        // rlwinm rA, rS, SH, MB, ME acts as a shift left when it clears the low SH bits
        // and keeps everything above them (ME == 31 - SH).
        shift = insn.operands[2];
        return insn.operands[4] == 31 - shift;
    }

    std::optional<SwitchTable> Analyse(const Image& image, const uint32_t* code, uint32_t bctrAddr, size_t maxBack)
    {
        ppc_insn insn;

        // Find the bounds check: cmplwi crN, rIdx, max followed by bgt/ble crN.
        size_t cmpBack = 0;
        uint32_t cr = UINT32_MAX;
        SwitchTable table{};
        table.bctr = bctrAddr;

        for (size_t i = 1; i <= maxBack; i++)
        {
            ppc::Disassemble(&code[-(ptrdiff_t)i], bctrAddr - 4 * i, insn);
            if (insn.opcode == nullptr)
                return std::nullopt;

            auto id = insn.opcode->id;
            if (id == PPC_INST_B && cr == UINT32_MAX && i + 1 <= maxBack)
            {
                // "ble crN, ok; b default; ok:" — the inverted form of "bgt crN, default".
                ppc_insn prev;
                uint32_t bAddr = bctrAddr - 4 * i;
                ppc::Disassemble(&code[-(ptrdiff_t)(i + 1)], bAddr - 4, prev);
                if (prev.opcode != nullptr && prev.opcode->id == PPC_INST_BLE && prev.operands[1] == bAddr + 4)
                {
                    table.defaultLabel = insn.operands[0];
                    cr = prev.operands[0];
                    i++;
                    continue;
                }
            }

            if (id == PPC_INST_BCTR || id == PPC_INST_BLR || id == PPC_INST_B || id == PPC_INST_BCTRL || id == PPC_INST_BL)
                return std::nullopt;

            if (cr == UINT32_MAX && (id == PPC_INST_BGT || id == PPC_INST_BGTLR))
            {
                cr = insn.operands[0];
                if (id == PPC_INST_BGT)
                    table.defaultLabel = insn.operands[1];
            }
            else if (cr != UINT32_MAX && id == PPC_INST_CMPLWI && insn.operands[0] == cr)
            {
                table.r = insn.operands[1];
                table.labels.resize(insn.operands[2] + 1);
                cmpBack = i;
                break;
            }
        }

        if (cmpBack == 0)
            return std::nullopt;

        // Symbolically execute from the bounds check up to the bctr.
        Value regs[32]{};
        regs[table.r] = { Kind::Index, 0, 0 };

        // The compiler sometimes copies the index right around the check ("mr r4, r3; cmplwi r3, N"),
        // then dispatches on the copy. Treat such a copy as the index too.
        for (size_t i = cmpBack + 1; i <= std::min(maxBack, cmpBack + 6); i++)
        {
            ppc::Disassemble(&code[-(ptrdiff_t)i], bctrAddr - 4 * i, insn);
            if (insn.opcode == nullptr || insn.opcode->id != PPC_INST_MR)
                continue;
            if (insn.operands[1] == table.r || insn.operands[0] == table.r)
            {
                uint32_t other = insn.operands[0] == table.r ? insn.operands[1] : insn.operands[0];
                bool clobbered = false;
                for (size_t k = i - 1; k > cmpBack; k--)
                {
                    ppc_insn mid;
                    ppc::Disassemble(&code[-(ptrdiff_t)k], bctrAddr - 4 * k, mid);
                    if (mid.opcode == nullptr || (mid.opcode->operands[0] != 0 && (mid.operands[0] == other || mid.operands[0] == table.r)))
                        clobbered = true;
                }
                if (!clobbered)
                    regs[other] = regs[table.r];
                break;
            }
        }

        std::optional<Value> ctrValue;

        for (size_t i = cmpBack - 1; i >= 1; i--)
        {
            uint32_t addr = bctrAddr - 4 * i;
            ppc::Disassemble(&code[-(ptrdiff_t)i], addr, insn);
            if (insn.opcode == nullptr)
                return std::nullopt;

            const auto* op = insn.operands;
            Value result{};
            int dst = -1;

            switch (insn.opcode->id)
            {
            case PPC_INST_NOP:
            case PPC_INST_BGT:
            case PPC_INST_BLE:
            case PPC_INST_BGTLR:
            case PPC_INST_BLELR:
                continue;

            case PPC_INST_B:
                // Only the out-of-range branch of the "ble ok; b default" form is allowed here.
                if (op[0] != table.defaultLabel)
                    return std::nullopt;
                continue;

            case PPC_INST_LIS:
                dst = op[0];
                result = { Kind::Const, op[1] << 16 };
                break;

            case PPC_INST_LI:
                dst = op[0];
                result = { Kind::Const, uint32_t(int32_t(int16_t(op[1]))) };
                break;

            case PPC_INST_ADDI:
                dst = op[0];
                if (regs[op[1]].kind == Kind::Const)
                    result = { Kind::Const, regs[op[1]].value + uint32_t(int32_t(int16_t(op[2]))) };
                break;

            case PPC_INST_ORI:
                dst = op[0];
                if (regs[op[1]].kind == Kind::Const)
                    result = { Kind::Const, regs[op[1]].value | op[2] };
                break;

            case PPC_INST_MR:
                dst = op[0];
                result = regs[op[1]];
                break;

            case PPC_INST_RLWINM:
            {
                dst = op[0];
                uint32_t shift;
                const auto& src = regs[op[1]];
                if (IsRotateLeftMask(insn, shift) && (src.kind == Kind::Index || src.kind == Kind::Entry))
                {
                    result = src;
                    result.shift += shift;
                }
                break;
            }

            case PPC_INST_LWZX:
            case PPC_INST_LHZX:
            case PPC_INST_LBZX:
            {
                dst = op[0];
                uint32_t size = insn.opcode->id == PPC_INST_LWZX ? 4 : insn.opcode->id == PPC_INST_LHZX ? 2 : 1;
                const Value* a = &regs[op[1]];
                const Value* b = &regs[op[2]];
                if (a->kind == Kind::Index)
                    std::swap(a, b);

                uint32_t expectedShift = size == 4 ? 2 : size == 2 ? 1 : 0;
                if (a->kind == Kind::Const && b->kind == Kind::Index && b->shift == expectedShift)
                {
                    result.kind = size == 4 ? Kind::Target : Kind::Entry;
                    result.tableAddr = a->value;
                    result.entrySize = size;
                    result.value = 0;
                    result.shift = 0;
                }
                break;
            }

            case PPC_INST_ADD:
            {
                dst = op[0];
                const Value* a = &regs[op[1]];
                const Value* b = &regs[op[2]];
                if (a->kind == Kind::Entry)
                    std::swap(a, b);
                if (a->kind == Kind::Const && b->kind == Kind::Entry)
                {
                    result = *b;
                    result.kind = Kind::Target;
                    result.value = a->value;
                }
                break;
            }

            case PPC_INST_MTCTR:
                ctrValue = regs[op[0]];
                continue;

            default:
                // Any other instruction writing a register we track invalidates it.
                // Conservatively bail on stores/branches/calls; for ALU ops assume operand 0 is the destination.
                if (insn.opcode->id == PPC_INST_B || insn.opcode->id == PPC_INST_BL || insn.opcode->id == PPC_INST_BCTRL)
                    return std::nullopt;
                if (insn.opcode->operands[0] != 0 && op[0] < 32)
                    dst = op[0];
                break;
            }

            if (dst >= 0)
                regs[dst] = result;
        }

        if (!ctrValue || ctrValue->kind != Kind::Target)
            return std::nullopt;

        // The index register must survive until the bctr so the recompiled switch reads it.
        if (regs[table.r].kind != Kind::Index || regs[table.r].shift != 0)
            return std::nullopt;

        table.entrySize = ctrValue->entrySize;
        for (size_t i = 0; i < table.labels.size(); i++)
        {
            uint32_t entryAddr = ctrValue->tableAddr + i * ctrValue->entrySize;
            const void* p = image.Find(entryAddr);
            if (p == nullptr)
                return std::nullopt;

            uint32_t entry;
            switch (ctrValue->entrySize)
            {
            case 4: entry = *(const be<uint32_t>*)p; break;
            case 2: entry = *(const be<uint16_t>*)p; break;
            default: entry = *(const uint8_t*)p; break;
            }

            table.labels[i] = ctrValue->value + (entry << ctrValue->shift);
        }

        // XenonRecomp arms the switch when it reaches `base` and fires on the next bctr.
        table.base = bctrAddr - 4 * cmpBack;
        return table;
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fmt::println("Usage: nb_switch_tables [input XEX] [output TOML]");
        return EXIT_FAILURE;
    }

    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());

    std::string out = "# Generated by nb_switch_tables\n";
    size_t bctrs = 0, found = 0, outside = 0, sizes[5]{};

    for (const auto& section : image.sections)
    {
        if (!(section.flags & SectionFlags_Code))
            continue;

        auto* code = (const uint32_t*)section.data;
        size_t count = section.size / 4;
        ppc_insn insn;

        for (size_t i = 0; i < count; i++)
        {
            ppc::Disassemble(&code[i], section.base + i * 4, insn);
            if (insn.opcode == nullptr || insn.opcode->id != PPC_INST_BCTR)
                continue;

            bctrs++;
            uint32_t addr = uint32_t(section.base + i * 4);
            auto table = Analyse(image, &code[i], addr, std::min<size_t>(i, 32));
            if (!table)
                continue;

            found++;
            sizes[table->entrySize]++;

            // Sanity check: labels should land inside the code section near the dispatch.
            for (auto label : table->labels)
            {
                if (label < section.base || label >= section.base + section.size)
                {
                    outside++;
                    break;
                }
            }

            out += fmt::format("# bctr 0x{:X}\n", table->bctr);
            out += fmt::format("[[switch]]\nbase = 0x{:X}\nr = {}\ndefault = 0x{:X}\nlabels = [\n", table->base, table->r, table->defaultLabel);
            for (auto label : table->labels)
                out += fmt::format("    0x{:X},\n", label);
            out += "]\n\n";
        }
    }

    std::ofstream(argv[2]).write(out.data(), out.size());
    fmt::println("bctr: {}, jump tables: {} (word {}, half {}, byte {}), tables with labels outside .text: {}",
        bctrs, found, sizes[4], sizes[2], sizes[1], outside);
    return EXIT_SUCCESS;
}
