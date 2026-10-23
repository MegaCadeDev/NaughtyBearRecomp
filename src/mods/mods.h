// Mod support: replacement models for the game's characters and meshes. No mods ship with
// the project; players put their own in the mods folder (see docs/modding.md).
#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mods
{
    // A model loaded from a .glb/.gltf file, flattened to one triangle list in its rest
    // pose, with all its materials' colours in one texture.
    struct Model
    {
        std::vector<float> positions; // xyz per vertex
        std::vector<float> normals;   // xyz per vertex, empty if the file has none
        std::vector<float> texcoords; // uv per vertex, into the texture below
        std::vector<uint32_t> indices;

        // RGBA8; empty if the model has no materials.
        uint32_t textureWidth = 0;
        uint32_t textureHeight = 0;
        std::vector<uint8_t> texture;
        float brightLuminance = 1.0f; // the 95th percentile of the texture's luminance, 0-1

        uint32_t VertexCount() const { return uint32_t(positions.size() / 3); }
    };

    // What one of a replaced mesh's texture slots holds.
    enum class TextureSource : uint8_t
    {
        Auto,       // chosen from what the game's texture looks like
        Game,       // the game's texture
        Model,      // the model's colours
        FlatNormal, // a normal map with no bumps
        Black,
        White,
    };

    // What to draw instead of one of the game's meshes.
    struct MeshReplacement
    {
        std::string mod;           // the mod's name, for messages
        bool hide = false;         // draw nothing
        bool fitBounds = false;    // turn, scale and move the model onto the game mesh
        std::array<TextureSource, 16> textures{};
        bool matchBrightness = false; // scale the model's colours to the game mesh's
        float brightness = 1.0f;      // then multiply them by this
        std::shared_ptr<const Model> model;
    };

    // A character a mod can replace: every skinned mesh loaded from its data package.
    struct Character
    {
        std::string package; // e.g. "naughtybear", "ninjabear"
        std::string label;   // e.g. "Naughty Bear", "Ninja bears"
    };

    // An installed mod, as the mods menu shows it.
    struct ModInfo
    {
        std::string id;        // its folder or file name in the mods folder
        std::string name;
        std::string credits;   // author and license, from mod.toml or the model file
        bool isModel = true;   // a model that replaces a character; otherwise mod.toml meshes
        std::string character; // model mods: the character it replaces, empty if off
        bool enabled = true;   // mesh mods: on or off
        std::string meshes;    // mesh mods: what they replace, for display
    };

#if defined(__PROSPERO__)
    // The PS5 version has no mod support: nothing is ever replaced.
    inline const std::vector<Character>& Characters() { static const std::vector<Character> none; return none; }
    inline std::vector<ModInfo> List(const std::filesystem::path&) { return {}; }
    inline bool ShowMenu(const std::filesystem::path&) { return false; }
    inline void SaveSettings(const std::filesystem::path&, const std::vector<ModInfo>&, bool) {}
    inline void Init(const std::filesystem::path&) {}
    inline bool Active() { return false; }
    inline bool ReplacesCharacters() { return false; }
    inline const MeshReplacement* FindMesh(uint64_t) { return nullptr; }
    inline const MeshReplacement* FindCharacter(const std::string&) { return nullptr; }
    inline float BrightLuminance(const std::vector<uint8_t>&) { return 1.0f; }
    inline bool WriteGlb(const std::filesystem::path&, const std::vector<float>&, const std::vector<float>&, const std::vector<float>&,
        const std::vector<uint32_t>&) { return false; }
#else
    // The characters of the game's data, in menu order.
    const std::vector<Character>& Characters();


    // Installed mods, in load order, with the characters they replace (one mod per
    // character).
    std::vector<ModInfo> List(const std::filesystem::path& directory);

    // The mods menu's settings, kept in mods/settings.toml.
    bool ShowMenu(const std::filesystem::path& directory);
    void SaveSettings(const std::filesystem::path& directory, const std::vector<ModInfo>& mods, bool showMenu);

    // Loads the enabled mods.
    void Init(const std::filesystem::path& directory);

    bool Active();
    bool ReplacesCharacters();
    const MeshReplacement* FindMesh(uint64_t id);
    const MeshReplacement* FindCharacter(const std::string& package);

    // The 95th percentile of an RGBA8 image's luminance (0-1): how bright its bright parts are.
    float BrightLuminance(const std::vector<uint8_t>& rgba);

    // Writes a mesh as a binary glTF (.glb). Normals and texcoords may be empty.
    bool WriteGlb(const std::filesystem::path& path, const std::vector<float>& positions, const std::vector<float>& normals,
        const std::vector<float>& texcoords, const std::vector<uint32_t>& indices);
#endif
}
