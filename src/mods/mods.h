// Mod support: replacement models for the game's meshes. No mods ship with the project;
// players put their own in the mods folder (see docs/modding.md).
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mods
{
    // A model loaded from a .glb/.gltf file, flattened to one triangle list.
    struct Model
    {
        std::vector<float> positions; // xyz per vertex
        std::vector<float> normals;   // xyz per vertex, empty if the file has none
        std::vector<float> texcoords; // uv per vertex, empty if the file has none
        std::vector<uint32_t> indices;

        // The base colour texture of the first material that has one, as RGBA8.
        uint32_t textureWidth = 0;
        uint32_t textureHeight = 0;
        std::vector<uint8_t> texture;

        uint32_t VertexCount() const { return uint32_t(positions.size() / 3); }
    };

    // What to draw instead of one of the game's meshes.
    struct MeshReplacement
    {
        std::string mod;          // the mod's folder name, for messages
        bool hide = false;        // draw nothing
        bool fitBounds = false;   // scale and move the model onto the game mesh's bounds
        int32_t textureSlot = 0;  // game texture the model's texture replaces; -1 keeps the game's
        std::shared_ptr<const Model> model;
    };

    // Loads every mod in the folder (each a subfolder with a mod.toml).
    void Init(const std::filesystem::path& directory);

    bool Active();
    const MeshReplacement* FindMesh(uint64_t id);

    // Writes a mesh as a binary glTF (.glb). Normals and texcoords may be empty.
    bool WriteGlb(const std::filesystem::path& path, const std::vector<float>& positions, const std::vector<float>& normals,
        const std::vector<float>& texcoords, const std::vector<uint32_t>& indices);
}
