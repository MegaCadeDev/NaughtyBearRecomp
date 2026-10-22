// Print guest memory from the loaded image as a string and hex: peek [XEX] [address] [bytes=64]
#include <file.h>
#include <image.h>
#include <fmt/core.h>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 3)
        return 1;
    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());
    uint32_t addr = std::stoul(argv[2], nullptr, 16);
    size_t count = argc > 3 ? std::stoul(argv[3]) : 64;
    if (addr < image.base || addr + count > image.base + image.size)
        return 1;
    const uint8_t* p = image.data.get() + (addr - image.base);
    std::string text;
    for (size_t i = 0; i < count && p[i]; i++)
        text += (p[i] >= 32 && p[i] < 127) ? char(p[i]) : '.';
    fmt::println("\"{}\"", text);
    for (size_t i = 0; i < count; i++)
        fmt::print("{:02X}{}", p[i], (i % 16 == 15) ? "\n" : " ");
    fmt::println("");
}
