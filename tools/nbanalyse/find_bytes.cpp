// Find a big-endian byte pattern in the code sections: find_bytes [XEX] "e9 c1 ff 68"
#include <file.h>
#include <image.h>
#include <xbox.h>
#include <fmt/core.h>
#include <sstream>
#include <vector>
#include <cstring>

int main(int argc, char** argv)
{
    if (argc < 3)
        return 1;
    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());
    std::vector<uint8_t> pattern;
    std::istringstream ss(argv[2]);
    std::string byte;
    while (ss >> byte)
        pattern.push_back(uint8_t(std::stoul(byte, nullptr, 16)));
    for (const auto& section : image.sections)
    {
        if (!(section.flags & SectionFlags_Code))
            continue;
        for (size_t i = 0; i + pattern.size() <= section.size; i += 4)
            if (memcmp(section.data + i, pattern.data(), pattern.size()) == 0)
                fmt::println("0x{:X}", section.base + i);
    }
}
