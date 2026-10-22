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
#include <algorithm>
#include <iterator>
#include <map>
#include <set>
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
        uint32_t tableAddr = 0;
        std::vector<uint32_t> labels;
    };

    bool IsRotateLeftMask(const ppc_insn& insn, uint32_t& shift)
    {
        // rlwinm rA, rS, SH, MB, ME acts as a shift left when it clears the low SH bits
        // and keeps everything above them (ME == 31 - SH).
        shift = insn.operands[2];
        return insn.operands[4] == 31 - shift;
    }

    // For a switch with no bounds check next to it: the index register and window to execute,
    // plus the limits used to decide how many table entries are real.
    struct UnboundedHint
    {
        uint32_t r;
        size_t startBack;
        uint32_t labelLo, labelHi; // labels must land in [lo, hi)
        uint32_t tableLimit;       // the next known table starts here
    };

    std::optional<SwitchTable> Analyse(const Image& image, const uint32_t* code, uint32_t bctrAddr, size_t maxBack, const UnboundedHint* hint = nullptr)
    {
        ppc_insn insn;

        // Find the bounds check: cmplwi crN, rIdx, max followed by bgt/ble crN.
        size_t cmpBack = 0;
        uint32_t cr = UINT32_MAX;
        SwitchTable table{};
        table.bctr = bctrAddr;

        if (hint != nullptr)
        {
            table.r = hint->r;
            cmpBack = hint->startBack;
        }

        for (size_t i = 1; hint == nullptr && i <= maxBack; i++)
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
        for (size_t i = cmpBack + 1; hint == nullptr && i <= std::min(maxBack, cmpBack + 6); i++)
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
        table.tableAddr = ctrValue->tableAddr;
        if (hint != nullptr)
            table.labels.resize(4096);

        for (size_t i = 0; i < table.labels.size(); i++)
        {
            uint32_t entryAddr = ctrValue->tableAddr + i * ctrValue->entrySize;
            const void* p = image.Find(entryAddr);
            if (hint != nullptr && (p == nullptr || entryAddr >= hint->tableLimit))
            {
                table.labels.resize(i);
                break;
            }
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

            // Without a bounds check, the table is as long as its entries keep landing in the function.
            // Extra entries are harmless (those cases are never taken); missing ones would crash.
            if (hint != nullptr && (table.labels[i] < hint->labelLo || table.labels[i] >= hint->labelHi || (table.labels[i] & 3) != 0))
            {
                table.labels.resize(i);
                break;
            }
        }

        if (table.labels.empty())
            return std::nullopt;

        // XenonRecomp arms the switch when it reaches `base` and fires on the next bctr.
        table.base = bctrAddr - 4 * cmpBack;
        return table;
    }
}

namespace
{
    // Address ranges covered by .pdata. Functions there already have exact sizes.
    struct PdataRanges
    {
        std::map<uint32_t, uint32_t> ranges; // begin -> end

        explicit PdataRanges(const Image& image)
        {
            const auto* pdata = image.Find(".pdata");
            if (pdata == nullptr)
                return;
            auto* pf = (const IMAGE_CE_RUNTIME_FUNCTION*)pdata->data;
            for (size_t i = 0; i < pdata->size / sizeof(IMAGE_CE_RUNTIME_FUNCTION); i++)
            {
                IMAGE_CE_RUNTIME_FUNCTION fn = pf[i];
                fn.BeginAddress = ByteSwap(fn.BeginAddress);
                fn.Data = ByteSwap(fn.Data);
                ranges[fn.BeginAddress] = fn.BeginAddress + fn.FunctionLength * 4;
            }
        }

        bool Contains(uint32_t addr) const
        {
            auto it = ranges.upper_bound(addr);
            return it != ranges.begin() && addr < std::prev(it)->second;
        }

        bool IsStart(uint32_t addr) const { return ranges.count(addr) != 0; }

        // The .pdata function containing addr, or the gap between functions it falls in.
        std::pair<uint32_t, uint32_t> Enclosing(uint32_t addr) const
        {
            auto it = ranges.upper_bound(addr);
            uint32_t next = it != ranges.end() ? it->first : UINT32_MAX;
            if (it == ranges.begin())
                return { 0, next };
            auto prev = std::prev(it);
            if (addr < prev->second)
                return { prev->first, prev->second };
            return { prev->second, next };
        }
    };

    constexpr uint32_t kBlr = 0x4E800020;
    constexpr uint32_t kBctr = 0x4E800420;

    bool IsTerminator(uint32_t insn)
    {
        // blr, bctr, or b without link
        return insn == kBlr || insn == kBctr || ((insn >> 26) == 18 && (insn & 1) == 0);
    }

    std::optional<uint32_t> BranchTarget(uint32_t insn, uint32_t addr)
    {
        uint32_t op = insn >> 26;
        if (op == 16 && (insn & 2) == 0) // bc
            return addr + uint32_t(int32_t(int16_t(insn & 0xFFFC)));
        if (op == 18 && (insn & 2) == 0) // b
            return addr + uint32_t((int32_t(insn << 6) >> 6) & ~3);
        return std::nullopt;
    }

    // Bounds of a leaf function (no .pdata entry) that contains a jump table.
    // XenonRecomp's analyzer treats the bctr as a tail call and ends the function there,
    // cutting off the case bodies, so these need explicit boundaries.
    std::optional<std::pair<uint32_t, uint32_t>> LeafFunctionBounds(const Image& image, const Section& section, const PdataRanges& pdata, const SwitchTable& table)
    {
        auto read = [&](uint32_t addr) { return ByteSwap(*(const uint32_t*)(section.data + (addr - section.base))); };
        uint32_t sectionEnd = uint32_t(section.base + section.size);
        uint32_t tableEnd = table.tableAddr + uint32_t(table.labels.size()) * table.entrySize;
        auto inTable = [&](uint32_t addr) { return addr + 4 > table.tableAddr && addr < tableEnd; };

        // Start: walk back to the instruction after the previous terminator or padding word.
        // A terminator can also be an early return, so keep going while the block before it
        // branches forward past the terminator into the code we already have.
        auto walkBack = [&](uint32_t from)
            {
                uint32_t addr = from;
                for (int n = 0; n < 1024 && addr > section.base; n++)
                {
                    uint32_t insn = read(addr - 4);
                    if (insn == 0 || IsTerminator(insn) || pdata.Contains(addr - 4))
                        break;
                    addr -= 4;
                }
                return addr;
            };

        uint32_t start = walkBack(table.base);
        for (int n = 0; n < 64 && start > section.base + 4; n++)
        {
            uint32_t terminator = start - 4;
            if (read(terminator) == 0 || pdata.Contains(terminator))
                break;
            uint32_t blockStart = walkBack(terminator);
            bool branchesIn = false;
            for (uint32_t a = blockStart; a < terminator; a += 4)
                if (auto target = BranchTarget(read(a), a); target && *target >= start && *target <= table.bctr)
                    branchesIn = true;
            if (!branchesIn)
                break;
            start = blockStart;
        }

        // End: the first terminator past every branch target seen so far.
        uint32_t furthest = std::max(table.defaultLabel, *std::max_element(table.labels.begin(), table.labels.end()));
        uint32_t addr = table.bctr + 4;
        for (; addr < sectionEnd && addr < table.bctr + 0x10000; addr += 4)
        {
            if (inTable(addr))
                continue;
            if (pdata.IsStart(addr))
                return std::make_pair(start, addr);

            uint32_t insn = read(addr);
            if (auto target = BranchTarget(insn, addr); target && *target > furthest && *target < table.bctr + 0x10000 && (insn >> 26) == 16)
                furthest = *target;

            if (addr >= furthest && IsTerminator(insn))
                return std::make_pair(start, addr + 4);
        }

        return std::nullopt;
    }

    // Replace the text between the BEGIN/END markers in the main config.
    bool WriteFunctionsToConfig(const std::string& path, const std::string& body)
    {
        std::ifstream in(path);
        if (!in)
            return false;
        std::string text((std::istreambuf_iterator<char>(in)), {});
        const std::string begin = "# BEGIN nb_switch_tables functions\n";
        const std::string end = "# END nb_switch_tables functions\n";
        auto b = text.find(begin);
        auto e = text.find(end);
        if (b == std::string::npos || e == std::string::npos || e < b)
            return false;
        text.replace(b + begin.size(), e - b - begin.size(), body);
        std::ofstream(path).write(text.data(), text.size());
        return true;
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fmt::println("Usage: nb_switch_tables [input XEX] [output switch table TOML] [main config TOML to update]");
        return EXIT_FAILURE;
    }

    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());

    PdataRanges pdata(image);
    std::string out = "# Generated by nb_switch_tables\n";
    struct LeafFunction
    {
        uint32_t start, end, bctr, defaultLabel;
        uint32_t tableAddr, tableEnd;
        std::vector<uint32_t> labels;
        const Section* section;
    };
    std::vector<LeafFunction> leafFunctions;
    size_t bctrs = 0, found = 0, outside = 0, sizes[5]{};

    struct Found
    {
        SwitchTable table;
        const Section* section;
        bool unbounded;
    };
    std::vector<Found> tables;
    std::vector<std::pair<const Section*, size_t>> unresolved; // bctr index within section

    // Pass 1: switches with a bounds check.
    for (const auto& section : image.sections)
    {
        if (!(section.flags & SectionFlags_Code))
            continue;

        auto* code = (const uint32_t*)section.data;
        ppc_insn insn;
        for (size_t i = 0; i < section.size / 4; i++)
        {
            ppc::Disassemble(&code[i], section.base + i * 4, insn);
            if (insn.opcode == nullptr || insn.opcode->id != PPC_INST_BCTR)
                continue;

            bctrs++;
            if (auto table = Analyse(image, &code[i], uint32_t(section.base + i * 4), std::min<size_t>(i, 32)))
                tables.push_back({ std::move(*table), &section, false });
            else
                unresolved.emplace_back(&section, i);
        }
    }

    // Pass 2: switches whose index was validated elsewhere. Try short windows ending at the
    // bctr with each register as the index, and size the table from where its labels stop
    // landing in the function or where the next known table begins.
    std::set<uint32_t> tableStarts;
    for (const auto& t : tables)
        tableStarts.insert(t.table.tableAddr);

    size_t unbounded = 0;
    for (auto [section, i] : unresolved)
    {
        auto* code = (const uint32_t*)section->data;
        uint32_t addr = uint32_t(section->base + i * 4);
        auto [lo, hi] = pdata.Enclosing(addr);

        for (size_t startBack = 2; startBack <= std::min<size_t>(i, 24); startBack++)
        {
            std::optional<SwitchTable> match;
            int matches = 0;
            for (uint32_t r = 1; r < 32; r++)
            {
                UnboundedHint hint{ r, startBack, lo, hi, UINT32_MAX };
                auto table = Analyse(image, &code[i], addr, std::min<size_t>(i, 32), &hint);
                if (!table)
                    continue;

                // Re-read with the table capped at the next known table.
                auto next = tableStarts.upper_bound(table->tableAddr);
                hint.tableLimit = next != tableStarts.end() ? *next : UINT32_MAX;
                table = Analyse(image, &code[i], addr, std::min<size_t>(i, 32), &hint);
                if (!table)
                    continue;

                matches++;
                match = std::move(table);
            }

            if (matches == 1)
            {
                fmt::println("unbounded switch at 0x{:X}: index r{}, {} entries", addr, match->r, match->labels.size());
                tableStarts.insert(match->tableAddr);
                tables.push_back({ std::move(*match), section, true });
                unbounded++;
                break;
            }
            if (matches > 1)
                break;
        }
    }

    std::sort(tables.begin(), tables.end(), [](auto& a, auto& b) { return a.table.bctr < b.table.bctr; });

    for (const auto& [t, sectionPtr, isUnbounded] : tables)
    {
        const auto* table = &t;
        const auto& section = *sectionPtr;
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

        if (!pdata.Contains(table->bctr))
        {
            if (auto bounds = LeafFunctionBounds(image, section, pdata, *table))
                leafFunctions.push_back({ bounds->first, bounds->second, table->bctr, table->defaultLabel,
                    table->tableAddr, table->tableAddr + uint32_t(table->labels.size()) * table->entrySize, table->labels, &section });
            else
                fmt::println("warning: could not bound leaf function around switch at 0x{:X}", table->bctr);
        }

        out += fmt::format("# bctr 0x{:X}{}\n", table->bctr, isUnbounded ? " (no bounds check; size inferred)" : "");
        out += fmt::format("[[switch]]\nbase = 0x{:X}\nr = {}\ndefault = 0x{:X}\nlabels = [\n", table->base, table->r, table->defaultLabel);
        for (auto label : table->labels)
            out += fmt::format("    0x{:X},\n", label);
        out += "]\n\n";
    }

    std::ofstream(argv[2]).write(out.data(), out.size());
    fmt::println("bctr: {}, jump tables: {} (word {}, half {}, byte {}; {} without bounds check), tables with labels outside .text: {}",
        bctrs, found, sizes[4], sizes[2], sizes[1], unbounded, outside);

    // Neighbouring leaf functions can share tail-merged code: a common default case, or case
    // bodies that branch into another function's tail. Split the ranges at every branch target
    // that comes from a different range, repeating until stable. Branches out of a range then
    // compile to tail calls, and falling off the end of a range continues into the next one.
    std::map<uint32_t, std::string> splits; // start -> comment
    for (const auto& fn : leafFunctions)
        splits.emplace(fn.start, fmt::format("switch at 0x{:X}", fn.bctr));

    auto owner = [&](uint32_t addr) -> const LeafFunction*
        {
            // Ranges can overlap before splitting; the closest start wins.
            const LeafFunction* best = nullptr;
            for (const auto& fn : leafFunctions)
                if (addr >= fn.start && addr < fn.end && (best == nullptr || fn.start > best->start))
                    best = &fn;
            return best;
        };

    // A split range ends at the next split point or at the end of the function it was cut from.
    auto rangeEnd = [&](std::map<uint32_t, std::string>::const_iterator it)
        {
            uint32_t end = owner(it->first)->end;
            if (auto next = std::next(it); next != splits.end())
                end = std::min(end, next->first);
            return end;
        };

    for (bool changed = true; changed;)
    {
        changed = false;
        for (auto it = splits.cbegin(); it != splits.cend(); ++it)
        {
            uint32_t start = it->first, end = rangeEnd(it);
            const auto* fn = owner(start);
            auto read = [&](uint32_t addr) { return ByteSwap(*(const uint32_t*)(fn->section->data + (addr - fn->section->base))); };

            std::vector<uint32_t> targets;
            if (fn->bctr >= start && fn->bctr < end)
            {
                targets = fn->labels;
                targets.push_back(fn->defaultLabel);
            }
            for (uint32_t addr = start; addr < end; addr += 4)
            {
                if (addr + 4 > fn->tableAddr && addr < fn->tableEnd)
                    continue;
                if (auto target = BranchTarget(read(addr), addr))
                    targets.push_back(*target);
            }

            for (uint32_t target : targets)
            {
                if ((target >= start && target < end) || owner(target) == nullptr || splits.count(target))
                    continue;
                splits.emplace(target, fmt::format("code shared with range at 0x{:X}", start));
                changed = true;
            }
            if (changed)
                break;
        }
    }

    std::string functions = "functions = [\n";
    for (auto it = splits.cbegin(); it != splits.cend(); ++it)
        functions += fmt::format("    {{ address = 0x{:X}, size = 0x{:X} }}, # {}\n", it->first, rangeEnd(it) - it->first, it->second);
    functions += "]\n";
    fmt::println("leaf switch functions: {} (from {} switches)", splits.size(), leafFunctions.size());
    if (argc > 3 && !WriteFunctionsToConfig(argv[3], functions))
    {
        fmt::println("error: {} has no BEGIN/END nb_switch_tables functions markers", argv[3]);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
