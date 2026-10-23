#include <stdafx.h>
#include "shader_store.h"
#include "shader_cache_file.h"
#if defined(__PROSPERO__)
// No translator on the PS5 (it needs DXC): shaders come from the PC version's cache.
namespace shader_translator
{
    inline size_t ContainerSize(const uint8_t*, size_t) { return 0; }
    inline bool Translate(const uint8_t*, std::vector<uint8_t>&, uint32_t&, std::string& error) { error = "no translator"; return false; }
    inline uint64_t Version() { return 0; }
}
#else
#include <gpu/translator/shader_translator.h>
#endif
#include <kernel/memory.h>
#include <kernel/packages.h>
#include <os/logger.h>
#include <user/paths.h>
#include <zstd.h>
#include <map>

// The executable's image in guest memory (main.cpp); scanned for the shaders it embeds.
extern uint32_t g_imageBase;
extern uint32_t g_imageSize;

namespace shader_store
{
    namespace
    {
        using CacheHeader = shader_cache_file::Header;
        constexpr uint32_t CACHE_MAGIC = shader_cache_file::MAGIC;
        constexpr uint32_t CACHE_FORMAT = shader_cache_file::FORMAT;

        std::mutex g_mutex;
        ankerl::unordered_dense::map<uint64_t, std::unique_ptr<Shader>> g_shaders;
        uint64_t g_dataFingerprint = 0;
        std::atomic<bool> g_dirty = false;

        std::filesystem::path CachePath()
        {
            return GetSavePath(false).parent_path() / "cache" / "shaders.bin";
        }

#if defined(__PROSPERO__)
        // The PS5 can't translate shaders (the translator needs DXC): it uses the cache the PC
        // version built from the same game files, packaged with the title (its cache folder
        // is the title's own).
        std::filesystem::path FindCache()
        {
            return CachePath();
        }
#endif

        uint64_t DataFingerprint()
        {
            return shader_cache_file::DataFingerprint(GetGamePath());
        }

        void Save()
        {
            std::lock_guard lock(g_mutex);
            std::vector<shader_cache_file::Entry> entries;
            for (const auto& [hash, shader] : g_shaders)
                entries.push_back({ hash, shader->specConstantsMask, &shader->spirv });
            shader_cache_file::Write(CachePath(), shader_translator::Version(), g_dataFingerprint, entries);
        }

        // Saves the cache in the background whenever shaders were added.
        void SaverThread()
        {
            while (true)
            {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                if (g_dirty.exchange(false))
                    Save();
            }
        }

        void Insert(uint64_t hash, std::vector<uint8_t>&& spirv, uint32_t specConstantsMask)
        {
            auto shader = std::make_unique<Shader>();
            shader->spirv = std::move(spirv);
            shader->specConstantsMask = specConstantsMask;
            std::lock_guard lock(g_mutex);
            g_shaders[hash] = std::move(shader);
        }

        // Finds the shader containers in data (no copies of ones already cached or seen).
        void Scan(const uint8_t* data, size_t size, std::map<uint64_t, std::vector<uint8_t>>& found)
        {
            shader_cache_file::Scan(data, size, shader_translator::ContainerSize, [](uint64_t hash) { return Find(hash) != nullptr; }, found);
        }
    }

    void Init()
    {
#if defined(__PROSPERO__)
        auto path = FindCache();
#else
        auto path = CachePath();
#endif
        uint64_t translatorVersion = shader_translator::Version();
        std::ifstream file(path, std::ios::binary);
        CacheHeader header{};
        if (file.read((char*)&header, sizeof(header)) && header.magic == CACHE_MAGIC && header.format == CACHE_FORMAT
            && (translatorVersion == 0 || header.translatorVersion == translatorVersion))
        {
            std::vector<uint8_t> compressed((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            std::vector<uint8_t> body(header.uncompressedSize);
            size_t size = ZSTD_decompress(body.data(), body.size(), compressed.data(), compressed.size());
            if (!ZSTD_isError(size) && size == body.size())
            {
                size_t offset = 0;
                for (uint32_t i = 0; i < header.count && offset + 16 <= body.size(); i++)
                {
                    uint64_t hash;
                    uint32_t mask, spirvSize;
                    memcpy(&hash, body.data() + offset, 8);
                    memcpy(&mask, body.data() + offset + 8, 4);
                    memcpy(&spirvSize, body.data() + offset + 12, 4);
                    offset += 16;
                    if (offset + spirvSize > body.size())
                        break;
                    Insert(hash, std::vector<uint8_t>(body.begin() + offset, body.begin() + offset + spirvSize), mask);
                    offset += spirvSize;
                }
                g_dataFingerprint = header.dataFingerprint;
                LOGFN("Shaders: {} translated shaders in {}", g_shaders.size(), path.string());
            }
        }
        else if (std::filesystem::exists(path))
        {
            LOGN("Shaders: the cache is from another translator version; it will be rebuilt");
        }

#if defined(__PROSPERO__)
        if (g_shaders.empty())
            LOGFN_ERROR("Shaders: no shader cache at {}; build it with the PC version from the same game files (see ps5/README.md)", path.string());
#else
        std::thread(SaverThread).detach();
#endif
    }

    void Precache(const std::function<void(float)>& progress)
    {
#if defined(__PROSPERO__)
        return; // the cache comes from the PC version
#endif
        uint64_t fingerprint = DataFingerprint();
        if (fingerprint == g_dataFingerprint)
            return;

        LOGN("Shaders: preparing the game's shaders (first run)...");
        auto start = std::chrono::steady_clock::now();
        progress(0.0f);

        // Find every shader container: in the executable, then in each data package.
        std::map<uint64_t, std::vector<uint8_t>> found;
        Scan((const uint8_t*)g_memory.Translate(g_imageBase), g_imageSize, found);

        std::vector<std::filesystem::path> packageFiles;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(GetGamePath() / "lu", ec))
        {
            if (entry.path().extension() == ".lu")
                packageFiles.push_back(entry.path());
        }
        std::sort(packageFiles.begin(), packageFiles.end());

        for (size_t i = 0; i < packageFiles.size(); i++)
        {
            std::vector<uint8_t> contents;
            if (packages::Read(packageFiles[i], contents))
                Scan(contents.data(), contents.size(), found);
            if ((i % 16) == 0)
                progress(0.4f * float(i) / float(packageFiles.size()));
        }

        // Translate them on all cores.
        std::vector<std::pair<uint64_t, std::vector<uint8_t>>> work(found.begin(), found.end());
        std::atomic<size_t> next = 0, done = 0, failed = 0;
        std::vector<std::thread> workers;
        uint32_t threadCount = std::max(1u, std::thread::hardware_concurrency());
        for (uint32_t t = 0; t < threadCount; t++)
        {
            workers.emplace_back([&]
                {
                    for (size_t index; (index = next++) < work.size();)
                    {
                        std::vector<uint8_t> spirv;
                        uint32_t mask = 0;
                        std::string error;
                        if (shader_translator::Translate(work[index].second.data(), spirv, mask, error))
                            Insert(work[index].first, std::move(spirv), mask);
                        else
                        {
                            failed++;
                            LOGFN_WARNING("Shaders: {:016x} could not be translated: {}", work[index].first, error);
                        }
                        done++;
                    }
                });
        }
        while (done < work.size())
        {
            progress(0.4f + 0.6f * (work.empty() ? 1.0f : float(done) / float(work.size())));
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
        for (auto& worker : workers)
            worker.join();
        progress(1.0f);

        {
            std::lock_guard lock(g_mutex);
            g_dataFingerprint = fingerprint;
        }
        Save();

        auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        LOGFN("Shaders: translated {} shaders ({} failed) from {} packages in {:.1f} s", work.size() - failed, failed.load(),
            packageFiles.size(), seconds);
    }

    const Shader* Find(uint64_t hash)
    {
        std::lock_guard lock(g_mutex);
        auto it = g_shaders.find(hash);
        return it != g_shaders.end() ? it->second.get() : nullptr;
    }

    void Ensure(const uint8_t* container, uint64_t hash)
    {
        if (container == nullptr || hash == 0 || Find(hash) != nullptr)
            return;
#if defined(__PROSPERO__)
        LOGFN_WARNING("Shaders: {:016x} isn't in the shader cache", hash);
        return;
#endif
        std::vector<uint8_t> spirv;
        uint32_t mask = 0;
        std::string error;
        if (shader_translator::Translate(container, spirv, mask, error))
        {
            Insert(hash, std::move(spirv), mask);
            g_dirty = true;
            LOGFN("Shaders: translated {:016x} on demand", hash);
        }
        else
        {
            LOGFN_WARNING("Shaders: {:016x} could not be translated: {}", hash, error);
        }
    }
}
