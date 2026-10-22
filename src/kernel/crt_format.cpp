// The kernel's printf family (sprintf, _snprintf, _vsnprintf, swprintf, _snwprintf,
// vswprintf, _vscwprintf) and DbgPrint, formatting with guest arguments.
//
// The game builds strings with these exports, so they have to produce real output.
// Unleashed Recompiled leaves them as unhooked stubs.
//
// Variadic arguments occupy 8-byte slots: the first eight are r3-r10, the rest live in the
// caller's parameter area at r1 + 0x10 + 8 * index (the same layout kernel/function.h
// reads stack arguments from). A va_list is a guest pointer to the next such slot.
// Integers are the low 32 bits of a slot unless a 64-bit size is given; floating point
// values are the slot's 64 bits as a double.
//
// Microsoft conventions apply: in the wide functions %s is a wide string and %S/%hs narrow,
// and the reverse in the narrow functions.

#include <stdafx.h>
#include <cpu/ppc_context.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <os/logger.h>
#include <bit>

namespace
{
    // Reads successive argument slots.
    struct ArgReader
    {
        const PPCContext* ctx = nullptr; // registers + stack, starting at slot `index`
        uint32_t index = 0;
        uint32_t vaList = 0;             // or a guest va_list

        uint64_t Next()
        {
            if (vaList != 0)
            {
                uint64_t value = ByteSwap(*(uint64_t*)g_memory.Translate(vaList));
                vaList += 8;
                return value;
            }

            uint32_t i = index++;
            if (i < 8)
            {
                const PPCRegister* gprs[] = { &ctx->r3, &ctx->r4, &ctx->r5, &ctx->r6, &ctx->r7, &ctx->r8, &ctx->r9, &ctx->r10 };
                return gprs[i]->u64;
            }
            return ByteSwap(*(uint64_t*)g_memory.Translate(ctx->r1.u32 + 0x10 + i * 8));
        }
    };

    // Format characters are either narrow bytes or big-endian UTF-16 units in guest memory.
    struct GuestText
    {
        const uint8_t* ptr;
        bool wide;

        char16_t At(size_t i) const
        {
            return wide ? char16_t(ByteSwap(((const uint16_t*)ptr)[i])) : char16_t(ptr[i]);
        }
    };

    void AppendNarrowString(std::u16string& out, uint32_t guest, int precision)
    {
        if (guest == 0)
        {
            out += u"(null)";
            return;
        }
        const char* s = (const char*)g_memory.Translate(guest);
        for (int i = 0; s[i] && (precision < 0 || i < precision); i++)
            out += char16_t((uint8_t)s[i]);
    }

    void AppendWideString(std::u16string& out, uint32_t guest, int precision)
    {
        if (guest == 0)
        {
            out += u"(null)";
            return;
        }
        const uint16_t* s = (const uint16_t*)g_memory.Translate(guest);
        for (int i = 0; s[i] && (precision < 0 || i < precision); i++)
            out += char16_t(ByteSwap(s[i]));
    }

    void AppendAscii(std::u16string& out, const char* s)
    {
        while (*s)
            out += char16_t((uint8_t)*s++);
    }

    void Pad(std::u16string& out, size_t start, int width, bool leftAlign)
    {
        size_t length = out.size() - start;
        if (width > 0 && length < size_t(width))
        {
            size_t padding = size_t(width) - length;
            if (leftAlign)
                out.append(padding, u' ');
            else
                out.insert(start, padding, u' ');
        }
    }

    std::u16string Format(GuestText format, ArgReader args, bool wideFunction)
    {
        std::u16string out;
        for (size_t i = 0; format.At(i) != 0; i++)
        {
            char16_t c = format.At(i);
            if (c != u'%')
            {
                out += c;
                continue;
            }

            // %[flags][width][.precision][size]type
            std::string flags;
            for (c = format.At(++i); c == u'-' || c == u'+' || c == u' ' || c == u'#' || c == u'0'; c = format.At(++i))
                flags += char(c);

            int width = -1;
            if (c == u'*')
            {
                width = int32_t(uint32_t(args.Next()));
                if (width < 0)
                {
                    flags += '-';
                    width = -width;
                }
                c = format.At(++i);
            }
            else
            {
                for (; c >= u'0' && c <= u'9'; c = format.At(++i))
                    width = (width < 0 ? 0 : width * 10) + (c - u'0');
            }

            int precision = -1;
            if (c == u'.')
            {
                precision = 0;
                c = format.At(++i);
                if (c == u'*')
                {
                    precision = int32_t(uint32_t(args.Next()));
                    c = format.At(++i);
                }
                else
                {
                    for (; c >= u'0' && c <= u'9'; c = format.At(++i))
                        precision = precision * 10 + (c - u'0');
                }
            }

            enum { SizeDefault, SizeShort, SizeChar, SizeLong, SizeLongLong, SizeWide } size = SizeDefault;
            if (c == u'h')
            {
                size = SizeShort;
                c = format.At(++i);
                if (c == u'h')
                {
                    size = SizeChar;
                    c = format.At(++i);
                }
            }
            else if (c == u'l')
            {
                size = SizeLong;
                c = format.At(++i);
                if (c == u'l')
                {
                    size = SizeLongLong;
                    c = format.At(++i);
                }
            }
            else if (c == u'w')
            {
                size = SizeWide;
                c = format.At(++i);
            }
            else if (c == u'L' || c == u'z' || c == u'j' || c == u't' || c == u'q')
            {
                size = (c == u'L') ? SizeDefault : SizeLongLong;
                c = format.At(++i);
            }
            else if (c == u'I')
            {
                if (format.At(i + 1) == u'6' && format.At(i + 2) == u'4')
                {
                    size = SizeLongLong;
                    i += 2;
                }
                else if (format.At(i + 1) == u'3' && format.At(i + 2) == u'2')
                {
                    i += 2;
                }
                c = format.At(++i);
            }

            if (c == 0)
                break;

            std::string spec = "%" + flags;
            if (width >= 0)
                spec += std::to_string(width);
            if (precision >= 0)
                spec += "." + std::to_string(precision);

            char buffer[512];
            size_t start = out.size();
            switch (c)
            {
            case u'%':
                out += u'%';
                break;

            case u'd':
            case u'i':
            {
                uint64_t raw = args.Next();
                long long value = size == SizeLongLong ? (long long)(int64_t)raw
                    : size == SizeShort ? (long long)(int16_t)raw
                    : size == SizeChar ? (long long)(int8_t)raw
                    : (long long)(int32_t)(uint32_t)raw;
                snprintf(buffer, sizeof(buffer), (spec + "lld").c_str(), value);
                AppendAscii(out, buffer);
                break;
            }

            case u'u':
            case u'o':
            case u'x':
            case u'X':
            {
                uint64_t raw = args.Next();
                unsigned long long value = size == SizeLongLong ? raw
                    : size == SizeShort ? (uint16_t)raw
                    : size == SizeChar ? (uint8_t)raw
                    : (uint32_t)raw;
                snprintf(buffer, sizeof(buffer), (spec + "ll" + char(c)).c_str(), value);
                AppendAscii(out, buffer);
                break;
            }

            case u'p':
                snprintf(buffer, sizeof(buffer), "%08X", uint32_t(args.Next()));
                AppendAscii(out, buffer);
                break;

            case u'f':
            case u'F':
            case u'e':
            case u'E':
            case u'g':
            case u'G':
            case u'a':
            case u'A':
                snprintf(buffer, sizeof(buffer), (spec + char(c)).c_str(), std::bit_cast<double>(args.Next()));
                AppendAscii(out, buffer);
                break;

            case u'c':
            case u'C':
            {
                char16_t ch = char16_t(args.Next());
                bool wideChar = size == SizeWide || size == SizeLong || (c == u'c') == wideFunction;
                out += wideChar ? ch : char16_t(uint8_t(ch));
                Pad(out, start, width, flags.find('-') != std::string::npos);
                break;
            }

            case u's':
            case u'S':
            {
                uint32_t guest = uint32_t(args.Next());
                bool wideString = size == SizeWide || size == SizeLong ? true
                    : size == SizeShort ? false
                    : (c == u's') == wideFunction;
                if (wideString)
                    AppendWideString(out, guest, precision);
                else
                    AppendNarrowString(out, guest, precision);
                Pad(out, start, width, flags.find('-') != std::string::npos);
                break;
            }

            case u'n':
            {
                uint32_t guest = uint32_t(args.Next());
                if (guest != 0)
                    *(be<uint32_t>*)g_memory.Translate(guest) = uint32_t(out.size());
                break;
            }

            default:
                out += u'%';
                out += c;
                break;
            }
        }
        return out;
    }

    // Writes narrow output with Microsoft _snprintf semantics when count is given.
    int32_t WriteNarrow(char* buffer, const std::u16string& text, int64_t count)
    {
        size_t length = text.size();
        if (count < 0)
            count = int64_t(length) + 1;
        size_t copy = std::min<size_t>(length, size_t(count));
        for (size_t i = 0; i < copy; i++)
            buffer[i] = char(text[i]);
        if (length < size_t(count))
        {
            buffer[length] = 0;
            return int32_t(length);
        }
        return length == size_t(count) ? int32_t(length) : -1;
    }

    int32_t WriteWide(be<uint16_t>* buffer, const std::u16string& text, int64_t count)
    {
        size_t length = text.size();
        if (count < 0)
            count = int64_t(length) + 1;
        size_t copy = std::min<size_t>(length, size_t(count));
        for (size_t i = 0; i < copy; i++)
            buffer[i] = uint16_t(text[i]);
        if (length < size_t(count))
        {
            buffer[length] = 0;
            return int32_t(length);
        }
        return length == size_t(count) ? int32_t(length) : -1;
    }

    ArgReader VarArgs(uint32_t firstSlot)
    {
        return ArgReader{ GetPPCContext(), firstSlot, 0 };
    }

    ArgReader VaList(uint32_t vaList)
    {
        return ArgReader{ nullptr, 0, vaList };
    }

    GuestText Narrow(const char* s) { return { (const uint8_t*)s, false }; }
    GuestText Wide(const be<uint16_t>* s) { return { (const uint8_t*)s, true }; }
}

// The variadic exports read their arguments straight from the guest context.

uint32_t sprintf_x(char* buffer, const char* format)
{
    return WriteNarrow(buffer, Format(Narrow(format), VarArgs(2), false), -1);
}

uint32_t _snprintf_x(char* buffer, uint32_t count, const char* format)
{
    return WriteNarrow(buffer, Format(Narrow(format), VarArgs(3), false), count);
}

uint32_t _vsnprintf_x(char* buffer, uint32_t count, const char* format, uint32_t vaList)
{
    return WriteNarrow(buffer, Format(Narrow(format), VaList(vaList), false), count);
}

uint32_t swprintf_x(be<uint16_t>* buffer, const be<uint16_t>* format)
{
    return WriteWide(buffer, Format(Wide(format), VarArgs(2), true), -1);
}

uint32_t _snwprintf_x(be<uint16_t>* buffer, uint32_t count, const be<uint16_t>* format)
{
    return WriteWide(buffer, Format(Wide(format), VarArgs(3), true), count);
}

uint32_t vswprintf_x(be<uint16_t>* buffer, const be<uint16_t>* format, uint32_t vaList)
{
    return WriteWide(buffer, Format(Wide(format), VaList(vaList), true), -1);
}

uint32_t _vscwprintf_x(const be<uint16_t>* format, uint32_t vaList)
{
    return uint32_t(Format(Wide(format), VaList(vaList), true).size());
}

uint32_t DbgPrint_x(const char* format)
{
    std::u16string text = Format(Narrow(format), VarArgs(1), false);
    std::string line;
    for (char16_t c : text)
        line += c < 0x80 ? char(c) : '?';
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    LOGF_IMPL(None, "DbgPrint", "{}", line);
    return 0;
}

GUEST_FUNCTION_HOOK(__imp__sprintf, sprintf_x);
GUEST_FUNCTION_HOOK(__imp___snprintf, _snprintf_x);
GUEST_FUNCTION_HOOK(__imp___vsnprintf, _vsnprintf_x);
GUEST_FUNCTION_HOOK(__imp__swprintf, swprintf_x);
GUEST_FUNCTION_HOOK(__imp___snwprintf, _snwprintf_x);
GUEST_FUNCTION_HOOK(__imp__vswprintf, vswprintf_x);
GUEST_FUNCTION_HOOK(__imp___vscwprintf, _vscwprintf_x);
GUEST_FUNCTION_HOOK(__imp__DbgPrint, DbgPrint_x);
