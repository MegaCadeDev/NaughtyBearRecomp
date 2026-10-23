// Mod support: replacement models for the game's meshes. No mods ship with the project;
// players put their own in the mods folder (see docs/modding.md).
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

        uint32_t VertexCount() const { return uint32_t(positions.size() / 3); }
    };

    // What one of a replaced mesh's texture slots holds.
    enum class TextureSource : uint8_t
    {
        Game,       // the game's texture
        Model,      // the model's colours
        FlatNormal, // a normal map with no bumps
        Black,
        White,
    };

    // What to draw instead of one of the game's meshes.
    struct MeshReplacement
    {
        std::string mod;          // the mod's name, for messages
        bool hide = false;        // draw nothing
        bool fitBounds = false;   // turn, scale and move the model onto the game mesh
        std::array<TextureSource, 16> textures{};
        std::shared_ptr<const Model> model;
    };

    // An installed mod, as the mods menu shows it.
    struct ModInfo
    {
        std::string id;      // its folder or file name in the mods folder
        std::string name;
        std::string target;  // what it replaces
        std::string credits; // author and license, from mod.toml or the model file
        bool enabled = true;
    };

    // Installed mods, in load order, and whether each is enabled.
    std::vector<ModInfo> List(const std::filesystem::path& directory);

    // The mods menu's settings, kept in mods/settings.toml.
    bool ShowMenu(const std::filesystem::path& directory);
    void SaveSettings(const std::filesystem::path& directory, const std::vector<ModInfo>& mods, bool showMenu);

    // Loads the enabled mods.
    void Init(const std::filesystem::path& directory);

    bool Active();
    const MeshReplacement* FindMesh(uint64_t id);

    // Writes a mesh as a binary glTF (.glb). Normals and texcoords may be empty.
    bool WriteGlb(const std::filesystem::path& path, const std::vector<float>& positions, const std::vector<float>& normals,
        const std::vector<float>& texcoords, const std::vector<uint32_t>& indices);
}
