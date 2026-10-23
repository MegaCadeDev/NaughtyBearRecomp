// Builds the translated shader cache (shaders.bin) from the game's files, without running
// the game: every shader container in Default.xex and in the data packages, translated to
// SPIR-V. The PS5 build runs it (the console can't translate shaders); the result is also
// a valid cache for the PC version.
//
// usage: nb_shader_precache <game folder or Default.xex> <output shaders.bin>
//
// The game decompresses its packages with its own XMemDecompress; here they're decompressed
// with libmspack's LZX decoder (xmem_lzx.cpp).
#include <gpu/shader_cache_file.h>
#include <gpu/translator/shader_translator.h>
#include <kernel/package_file.h>

#include "xmem_lzx.h"

#include <file.h>
#include <image.h>

#include <malloc.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace
{
    bool XMemDecompress(const uint8_t* source, uint32_t sourceSize, uint8_t* destination, uint32_t destinationSize)
    {
        return XMemDecompressLzx(source, sourceSize, destination, destinationSize, package_file::LZX_WINDOW_SIZE);
    }

    std::filesystem::path FindExecutable(const std::filesystem::path& path)
    {
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec))
            return path;
        for (auto it = std::filesystem::recursive_directory_iterator(path, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
                break;
            std::string name = it->path().filename().string();
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            if (name == "default.xex" && it->is_regular_file(ec))
                return it->path();
        }
        return {};
    }
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s <game folder or Default.xex> <output shaders.bin>\n", argv[0]);
        return 2;
    }
    auto start = std::chrono::steady_clock::now();
    auto xexPath = FindExecutable(argv[1]);
    if (xexPath.empty())
    {
        fprintf(stderr, "No Default.xex in %s\n", argv[1]);
        return 1;
    }
    auto gameDirectory = xexPath.parent_path();
    std::map<uint64_t, std::vector<uint8_t>> found;
    auto known = [](uint64_t) { return false; };

    // The executable's image, as the loader maps it.
    auto file = LoadFile(xexPath);
    auto image = Image::ParseImage(file.data(), file.size());
    if (!image.data)
    {
        fprintf(stderr, "Could not load %s\n", xexPath.string().c_str());
        return 1;
    }
    size_t imageSize = std::min<size_t>(image.size, malloc_usable_size(image.data.get()));
    shader_cache_file::Scan(image.data.get(), imageSize, shader_translator::ContainerSize, known, found);
    size_t executableShaders = found.size();

    // Every data package.
    std::vector<std::filesystem::path> packages;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(gameDirectory / "lu", ec))
        if (entry.path().extension() == ".lu")
            packages.push_back(entry.path());
    std::sort(packages.begin(), packages.end());
    size_t failedPackages = 0;
    for (const auto& path : packages)
    {
        std::vector<uint8_t> contents;
        auto result = package_file::Read(path, contents, XMemDecompress);
        if (result == package_file::Result::Ok)
            shader_cache_file::Scan(contents.data(), contents.size(), shader_translator::ContainerSize, known, found);
        else if (result != package_file::Result::NotAPackage)
        {
            fprintf(stderr, "Could not read %s (%d)\n", path.filename().string().c_str(), int(result));
            failedPackages++;
        }
    }
    printf("Found %zu shaders (%zu in the executable) in %zu packages\n", found.size(), executableShaders, packages.size());

    // Translate them on all cores.
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> work(found.begin(), found.end());
    struct Result
    {
        std::vector<uint8_t> spirv;
        uint32_t mask = 0;
        bool ok = false;
    };
    std::vector<Result> results(work.size());
    std::atomic<size_t> next = 0;
    std::vector<std::thread> workers;
    for (uint32_t t = 0; t < std::max(1u, std::thread::hardware_concurrency()); t++)
    {
        workers.emplace_back([&]
            {
                for (size_t index; (index = next++) < work.size();)
                {
                    std::string error;
                    results[index].ok = shader_translator::Translate(work[index].second.data(), results[index].spirv, results[index].mask, error);
                    if (!results[index].ok)
                        fprintf(stderr, "Shader %016llx could not be translated: %s\n", (unsigned long long)work[index].first, error.c_str());
                }
            });
    }
    for (auto& worker : workers)
        worker.join();

    std::vector<shader_cache_file::Entry> entries;
    for (size_t i = 0; i < work.size(); i++)
        if (results[i].ok)
            entries.push_back({ work[i].first, results[i].mask, &results[i].spirv });
    if (!shader_cache_file::Write(argv[2], shader_translator::Version(), shader_cache_file::DataFingerprint(gameDirectory), entries))
    {
        fprintf(stderr, "Could not write %s\n", argv[2]);
        return 1;
    }
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    printf("Translated %zu of %zu shaders into %s in %.1f s\n", entries.size(), work.size(), argv[2], seconds);
    return failedPackages != 0 ? 1 : 0;
}
