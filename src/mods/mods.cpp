#if !defined(__PROSPERO__) // the PS5 version has no mods

#include <stdafx.h>
#include "mods.h"
#include <os/logger.h>
#include <user/paths.h>
#include <cgltf.h>
#include <stb_image.h>
#include <toml.hpp>
#include <ankerl/unordered_dense.h>
#include <cmath>
#include <map>

// A mod is any of these in the mods folder:
//
//   - a model file (.glb or .gltf), replacing a character (Naughty Bear unless the mods menu
//     says otherwise);
//   - a folder with a model file and no mod.toml: the same;
//   - a folder with a mod.toml for a model:
//
//       name = "My bear"
//       author = "..."                 # shown in the mods menu
//       license = "CC-BY-4.0"
//       model = "bear.glb"             # .glb or .gltf in the mod's folder
//       character = "ninjabear"        # who it replaces by default (see CHARACTERS)
//       fit = "bounds"                 # or "none": the model is already lined up
//       textures = { 4 = "model" }     # per texture slot: auto, model, flat_normal, black,
//                                      # white or game (auto: chosen from the game's texture)
//       brightness = 1.0               # multiplies the model's colours, after they're matched
//                                      # to the game's
//       scale = 1.0                    # applied to the model first, then
//       rotate = [0, 0, 0]             # degrees about X, Y, Z, then
//       offset = [0, 0, 0]             # a translation
//
//   - a folder with a mod.toml replacing meshes by ID (from NB_MOD_EXPORT), with the same
//     model keys per mesh:
//
//       [[mesh]]
//       replace = "0123456789abcdef"   # or a list
//       model = "thing.glb"
//
//       [[mesh]]
//       replace = "fedcba9876543210"
//       hide = true                    # draw nothing for this mesh
//
// mods/settings.toml, written by the mods menu, says which mod replaces each character
// (only one can) and which mods are off. Folders whose name starts with "_" or "." (such as
// _export) are not mods.
namespace mods
{
    namespace
    {
        // The game's characters, by data package: every skinned mesh loaded from the
        // package is the character (and costume pieces in it).
        const Character CHARACTERS[] =
        {
            { "naughtybear", "Naughty Bear" },
            { "naughtyalien", "Naughty Bear (alien costume)" },
            { "naughtyarmy", "Naughty Bear (army costume)" },
            { "naughtycop", "Naughty Bear (cop costume)" },
            { "naughtydanger", "Naughty Bear (danger costume)" },
            { "naughtyninja", "Naughty Bear (ninja costume)" },
            { "naughtypirate", "Naughty Bear (pirate costume)" },
            { "naughtyrobot", "Naughty Bear (robot costume)" },
            { "naughtyswat", "Naughty Bear (SWAT costume)" },
            { "naughtyuni", "Naughty Bear (unibear costume)" },
            { "naughtyvampire", "Naughty Bear (vampire costume)" },
            { "naughtyzombie", "Naughty Bear (zombie costume)" },
            { "normalbear", "Island bears" },
            { "alienbear", "Alien bears" },
            { "armybear", "Army bears" },
            { "copbear", "Cop bears" },
            { "dangerbear", "Danger bears" },
            { "ninjabear", "Ninja bears" },
            { "piratebear", "Pirate bears" },
            { "robobear", "Robot bears" },
            { "swatbear", "SWAT bears" },
            { "unibear", "Unibear" },
            { "vampirebear", "Vampire bears" },
            { "zombiebear", "Zombie bears" },
            { "impersonatoralien", "Impersonator (alien)" },
            { "impersonatorarmy", "Impersonator (army)" },
            { "impersonatorcop", "Impersonator (cop)" },
            { "impersonatordanger", "Impersonator (danger)" },
            { "impersonatorninja", "Impersonator (ninja)" },
            { "impersonatorpirate", "Impersonator (pirate)" },
            { "impersonatorrobot", "Impersonator (robot)" },
            { "impersonatorswat", "Impersonator (SWAT)" },
            { "impersonatorvampire", "Impersonator (vampire)" },
            { "impersonatorzombie", "Impersonator (zombie)" },
            { "mp_naughtybear", "Naughty Bear (multiplayer)" },
            { "mp_redbear", "Red bear (multiplayer)" },
            { "mp_bluebear", "Blue bear (multiplayer)" },
            { "mp_greenbear", "Green bear (multiplayer)" },
            { "mp_yellowbear", "Yellow bear (multiplayer)" },
            { "mp_zombiemodecharacter", "Zombie (multiplayer)" },
        };
        constexpr const char* DEFAULT_CHARACTER = "naughtybear";

        ankerl::unordered_dense::map<uint64_t, MeshReplacement> g_meshes;
        ankerl::unordered_dense::map<std::string, MeshReplacement> g_characters;

        // ---- Finding mods ----

        bool IsModel(const std::filesystem::path& path)
        {
            auto extension = path.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
            return extension == ".glb" || extension == ".gltf";
        }

        struct ModSource
        {
            std::string id;
            std::filesystem::path directory; // where its files are
            std::filesystem::path manifest;  // mod.toml, or empty
            std::filesystem::path model;     // the model, without a manifest
        };

        std::vector<ModSource> FindMods(const std::filesystem::path& directory)
        {
            std::vector<ModSource> sources;
            std::error_code ec;
            std::vector<std::filesystem::path> entries;
            for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
                entries.push_back(entry.path());
            std::sort(entries.begin(), entries.end());
            for (const auto& path : entries)
            {
                std::string name = path.filename().string();
                if (name.empty() || name[0] == '_' || name[0] == '.')
                    continue;
                if (std::filesystem::is_directory(path, ec))
                {
                    ModSource source{ name, path };
                    if (std::filesystem::exists(path / "mod.toml", ec))
                    {
                        source.manifest = path / "mod.toml";
                    }
                    else
                    {
                        std::vector<std::filesystem::path> models;
                        for (const auto& file : std::filesystem::directory_iterator(path, ec))
                            if (file.is_regular_file() && IsModel(file.path()))
                                models.push_back(file.path());
                        if (models.empty())
                            continue;
                        std::sort(models.begin(), models.end());
                        source.model = models.front();
                    }
                    sources.push_back(std::move(source));
                }
                else if (IsModel(path))
                {
                    sources.push_back({ name, directory, {}, path });
                }
            }
            return sources;
        }

        std::optional<toml::table> ReadManifest(const std::filesystem::path& path)
        {
            try
            {
                return toml::parse_file(path.string());
            }
            catch (const toml::parse_error& error)
            {
                LOGFN_ERROR("Mods: {}: {}", path.string(), error.description());
                return std::nullopt;
            }
        }

        // ---- Settings ----

        struct Settings
        {
            bool showMenu = true;
            std::set<std::string> off;                    // mods turned off
            std::map<std::string, std::string> characters; // character -> mod
        };

        Settings LoadSettings(const std::filesystem::path& directory)
        {
            Settings settings;
            auto path = directory / "settings.toml";
            if (!std::filesystem::exists(path))
                return settings;
            if (auto table = ReadManifest(path))
            {
                settings.showMenu = (*table)["show_menu"].value_or(true);
                if (const toml::array* off = (*table)["off"].as_array())
                    for (const toml::node& id : *off)
                        if (auto text = id.value<std::string>())
                            settings.off.insert(*text);
                if (const toml::table* characters = (*table)["characters"].as_table())
                    for (const auto& [character, mod] : *characters)
                        if (auto text = mod.value<std::string>())
                            settings.characters[std::string(character.str())] = *text;
            }
            return settings;
        }

        // ---- Models ----

        struct Transform
        {
            float scale = 1.0f;
            float rotate[3] = {};
            float offset[3] = {};
        };

        // Rotation matrix for X, then Y, then Z rotations (degrees).
        void RotationMatrix(const float degrees[3], float m[9])
        {
            constexpr float toRadians = 3.14159265358979f / 180.0f;
            float cx = std::cos(degrees[0] * toRadians), sx = std::sin(degrees[0] * toRadians);
            float cy = std::cos(degrees[1] * toRadians), sy = std::sin(degrees[1] * toRadians);
            float cz = std::cos(degrees[2] * toRadians), sz = std::sin(degrees[2] * toRadians);
            // Rz * Ry * Rx
            m[0] = cz * cy; m[1] = cz * sy * sx - sz * cx; m[2] = cz * sy * cx + sz * sx;
            m[3] = sz * cy; m[4] = sz * sy * sx + cz * cx; m[5] = sz * sy * cx - cz * sx;
            m[6] = -sy;     m[7] = cy * sx;                m[8] = cy * cx;
        }

        void TransformModel(Model& model, const Transform& transform)
        {
            float r[9];
            RotationMatrix(transform.rotate, r);
            for (size_t i = 0; i + 2 < model.positions.size(); i += 3)
            {
                float p[3] = { model.positions[i] * transform.scale, model.positions[i + 1] * transform.scale, model.positions[i + 2] * transform.scale };
                for (int j = 0; j < 3; j++)
                    model.positions[i + j] = r[j * 3] * p[0] + r[j * 3 + 1] * p[1] + r[j * 3 + 2] * p[2] + transform.offset[j];
            }
            for (size_t i = 0; i + 2 < model.normals.size(); i += 3)
            {
                float n[3] = { model.normals[i], model.normals[i + 1], model.normals[i + 2] };
                for (int j = 0; j < 3; j++)
                    model.normals[i + j] = r[j * 3] * n[0] + r[j * 3 + 1] * n[1] + r[j * 3 + 2] * n[2];
            }
        }

        // Column-major 4x4 matrices, as glTF stores them.
        using Matrix = std::array<float, 16>;

        Matrix Multiply(const Matrix& a, const Matrix& b)
        {
            Matrix c{};
            for (int column = 0; column < 4; column++)
                for (int row = 0; row < 4; row++)
                    for (int k = 0; k < 4; k++)
                        c[column * 4 + row] += a[k * 4 + row] * b[column * 4 + k];
            return c;
        }

        struct Image
        {
            uint32_t width = 0;
            uint32_t height = 0;
            std::vector<uint8_t> pixels; // RGBA8
        };

        bool DecodeImage(const cgltf_image* image, const std::filesystem::path& directory, Image& out)
        {
            int width = 0, height = 0, channels = 0;
            stbi_uc* pixels = nullptr;
            if (image->buffer_view != nullptr && image->buffer_view->buffer->data != nullptr)
            {
                const auto* data = (const stbi_uc*)image->buffer_view->buffer->data + image->buffer_view->offset;
                pixels = stbi_load_from_memory(data, int(image->buffer_view->size), &width, &height, &channels, 4);
            }
            else if (image->uri != nullptr && strncmp(image->uri, "data:", 5) != 0)
            {
                std::string uri = image->uri;
                cgltf_decode_uri(uri.data());
                uri.resize(strlen(uri.c_str()));
                pixels = stbi_load((directory / uri).string().c_str(), &width, &height, &channels, 4);
            }
            if (pixels == nullptr)
                return false;
            out.width = uint32_t(width);
            out.height = uint32_t(height);
            out.pixels.assign(pixels, pixels + size_t(width) * height * 4);
            stbi_image_free(pixels);
            return true;
        }

        Image HalveImage(const Image& image)
        {
            Image half;
            half.width = std::max(1u, image.width / 2);
            half.height = std::max(1u, image.height / 2);
            half.pixels.resize(size_t(half.width) * half.height * 4);
            for (uint32_t y = 0; y < half.height; y++)
                for (uint32_t x = 0; x < half.width; x++)
                    for (uint32_t c = 0; c < 4; c++)
                    {
                        uint32_t sum = 0;
                        for (uint32_t dy = 0; dy < 2; dy++)
                            for (uint32_t dx = 0; dx < 2; dx++)
                            {
                                uint32_t sx = std::min(x * 2 + dx, image.width - 1), sy = std::min(y * 2 + dy, image.height - 1);
                                sum += image.pixels[(size_t(sy) * image.width + sx) * 4 + c];
                            }
                        half.pixels[(size_t(y) * half.width + x) * 4 + c] = uint8_t(sum / 4);
                    }
            return half;
        }

        // Packs the materials' images into one texture (rows of images, each with a border
        // repeating its edge pixels so filtering doesn't bleed), and moves each vertex's
        // texture coordinates into its material's image.
        void BuildAtlas(Model& model, std::vector<Image>& tiles, const std::vector<uint32_t>& vertexTiles)
        {
            constexpr uint32_t BORDER = 4;
            constexpr uint32_t MAX_SIZE = 4096;
            std::vector<uint32_t> order(tiles.size());
            std::iota(order.begin(), order.end(), 0u);
            std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return tiles[a].height > tiles[b].height; });

            std::vector<uint32_t> tileX(tiles.size()), tileY(tiles.size());
            uint32_t width = 0, height = 0;
            while (true)
            {
                uint64_t area = 0;
                uint32_t widest = 0;
                for (const Image& tile : tiles)
                {
                    area += uint64_t(tile.width + BORDER * 2) * (tile.height + BORDER * 2);
                    widest = std::max(widest, tile.width + BORDER * 2);
                }
                width = std::max(widest, std::bit_ceil(uint32_t(std::ceil(std::sqrt(double(area))))));
                uint32_t x = 0, y = 0, rowHeight = 0;
                for (uint32_t i : order)
                {
                    uint32_t w = tiles[i].width + BORDER * 2, h = tiles[i].height + BORDER * 2;
                    if (x + w > width)
                        x = 0, y += rowHeight, rowHeight = 0;
                    tileX[i] = x + BORDER;
                    tileY[i] = y + BORDER;
                    x += w;
                    rowHeight = std::max(rowHeight, h);
                }
                height = y + rowHeight;
                if (width <= MAX_SIZE && height <= MAX_SIZE)
                    break;
                for (Image& tile : tiles)
                    tile = HalveImage(tile);
            }

            model.textureWidth = width;
            model.textureHeight = height;
            model.texture.assign(size_t(width) * height * 4, 0);
            for (size_t i = 0; i < tiles.size(); i++)
            {
                const Image& tile = tiles[i];
                for (int32_t y = -int32_t(BORDER); y < int32_t(tile.height + BORDER); y++)
                    for (int32_t x = -int32_t(BORDER); x < int32_t(tile.width + BORDER); x++)
                    {
                        uint32_t sx = uint32_t(std::clamp(x, 0, int32_t(tile.width) - 1)), sy = uint32_t(std::clamp(y, 0, int32_t(tile.height) - 1));
                        memcpy(&model.texture[(size_t(tileY[i] + y) * width + tileX[i] + x) * 4], &tile.pixels[(size_t(sy) * tile.width + sx) * 4], 4);
                    }
            }

            for (uint32_t v = 0; v < model.VertexCount(); v++)
            {
                const Image& tile = tiles[vertexTiles[v]];
                float& u = model.texcoords[v * 2];
                float& t = model.texcoords[v * 2 + 1];
                // Repeating texture coordinates can't repeat within the atlas; wrap them.
                if (u < 0.0f || u > 1.0f)
                    u -= std::floor(u);
                if (t < 0.0f || t > 1.0f)
                    t -= std::floor(t);
                u = (float(tileX[vertexTiles[v]]) + u * float(tile.width)) / float(width);
                t = (float(tileY[vertexTiles[v]]) + t * float(tile.height)) / float(height);
            }
        }

        // The 95th percentile of an RGBA8 image's luminance (0-1), ignoring transparent pixels:
        // how bright its bright parts are.
        float ImageBrightness(const std::vector<uint8_t>& pixels)
        {
            uint32_t histogram[256] = {};
            uint64_t count = 0;
            for (size_t i = 0; i + 3 < pixels.size(); i += 4)
            {
                if (pixels[i + 3] == 0)
                    continue;
                histogram[(pixels[i] * 54 + pixels[i + 1] * 183 + pixels[i + 2] * 19) >> 8]++;
                count++;
            }
            uint64_t seen = 0;
            for (uint32_t level = 0; level < 256; level++)
            {
                seen += histogram[level];
                if (seen >= count * 95 / 100 && count != 0)
                    return float(level) / 255.0f;
            }
            return 1.0f;
        }

        // Every triangle primitive of every node, as the model looks at rest: skinned meshes
        // are posed by their skeleton (as model viewers show them), others placed by their
        // node.
        std::shared_ptr<Model> LoadModel(const std::filesystem::path& path)
        {
            cgltf_options options{};
            cgltf_data* data = nullptr;
            std::string file = path.string();
            if (cgltf_parse_file(&options, file.c_str(), &data) != cgltf_result_success)
            {
                LOGFN_ERROR("Mods: could not read {}", file);
                return nullptr;
            }
            std::unique_ptr<cgltf_data, decltype(&cgltf_free)> owner(data, cgltf_free);
            if (cgltf_load_buffers(&options, data, file.c_str()) != cgltf_result_success)
            {
                LOGFN_ERROR("Mods: could not load the buffers of {}", file);
                return nullptr;
            }

            auto model = std::make_shared<Model>();
            bool anyNormals = false;

            // One atlas tile per material: its base colour texture (times its colour factor),
            // or just its colour.
            std::vector<Image> tiles;
            std::map<const cgltf_material*, uint32_t> materialTiles;
            std::map<const cgltf_image*, Image> images;
            std::vector<uint32_t> vertexTiles;
            auto tileOf = [&](const cgltf_material* material) -> uint32_t
                {
                    if (auto it = materialTiles.find(material); it != materialTiles.end())
                        return it->second;
                    float factor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                    Image tile;
                    if (material != nullptr && material->has_pbr_metallic_roughness)
                    {
                        memcpy(factor, material->pbr_metallic_roughness.base_color_factor, sizeof(factor));
                        const cgltf_texture* texture = material->pbr_metallic_roughness.base_color_texture.texture;
                        if (texture != nullptr && texture->image != nullptr)
                        {
                            auto image = images.find(texture->image);
                            if (image == images.end())
                            {
                                Image decoded;
                                if (!DecodeImage(texture->image, path.parent_path(), decoded))
                                    LOGFN_WARNING("Mods: could not load a texture of {}", file);
                                image = images.emplace(texture->image, std::move(decoded)).first;
                            }
                            tile = image->second;
                        }
                    }
                    if (tile.pixels.empty())
                    {
                        tile.width = tile.height = 4;
                        tile.pixels.assign(4 * 4 * 4, 255);
                    }
                    for (size_t i = 0; i < tile.pixels.size(); i++)
                        tile.pixels[i] = uint8_t(std::clamp(float(tile.pixels[i]) * factor[i & 3], 0.0f, 255.0f));
                    tiles.push_back(std::move(tile));
                    return materialTiles[material] = uint32_t(tiles.size() - 1);
                };

            for (cgltf_size n = 0; n < data->nodes_count; n++)
            {
                const cgltf_node* node = &data->nodes[n];
                if (node->mesh == nullptr)
                    continue;
                Matrix world;
                cgltf_node_transform_world(node, world.data());

                // A skinned mesh's vertices are placed by its joints, not its node.
                std::vector<Matrix> joints;
                if (const cgltf_skin* skin = node->skin)
                {
                    for (cgltf_size j = 0; j < skin->joints_count; j++)
                    {
                        Matrix jointWorld, inverseBind = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
                        cgltf_node_transform_world(skin->joints[j], jointWorld.data());
                        if (skin->inverse_bind_matrices != nullptr)
                            cgltf_accessor_read_float(skin->inverse_bind_matrices, j, inverseBind.data(), 16);
                        joints.push_back(Multiply(jointWorld, inverseBind));
                    }
                }

                for (cgltf_size p = 0; p < node->mesh->primitives_count; p++)
                {
                    const cgltf_primitive& primitive = node->mesh->primitives[p];
                    if (primitive.type != cgltf_primitive_type_triangles)
                        continue;
                    const cgltf_accessor *position = nullptr, *normal = nullptr, *texcoord = nullptr, *jointIndices = nullptr, *weights = nullptr;
                    for (cgltf_size a = 0; a < primitive.attributes_count; a++)
                    {
                        const cgltf_attribute& attribute = primitive.attributes[a];
                        if (attribute.type == cgltf_attribute_type_position)
                            position = attribute.data;
                        else if (attribute.type == cgltf_attribute_type_normal)
                            normal = attribute.data;
                        else if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0)
                            texcoord = attribute.data;
                        else if (attribute.type == cgltf_attribute_type_joints && attribute.index == 0)
                            jointIndices = attribute.data;
                        else if (attribute.type == cgltf_attribute_type_weights && attribute.index == 0)
                            weights = attribute.data;
                    }
                    if (position == nullptr)
                        continue;
                    bool skinned = !joints.empty() && jointIndices != nullptr && weights != nullptr;
                    uint32_t tile = tileOf(primitive.material);

                    uint32_t first = model->VertexCount();
                    for (cgltf_size v = 0; v < position->count; v++)
                    {
                        Matrix m = world;
                        if (skinned)
                        {
                            cgltf_uint index[4] = {};
                            float weight[4] = {};
                            cgltf_accessor_read_uint(jointIndices, v, index, 4);
                            cgltf_accessor_read_float(weights, v, weight, 4);
                            float total = weight[0] + weight[1] + weight[2] + weight[3];
                            if (total > 0.0f)
                            {
                                m = {};
                                for (int k = 0; k < 4; k++)
                                    if (weight[k] > 0.0f && index[k] < joints.size())
                                        for (int e = 0; e < 16; e++)
                                            m[e] += joints[index[k]][e] * weight[k] / total;
                            }
                        }

                        float p3[3] = {}, n3[3] = { 0.0f, 0.0f, 1.0f }, uv[2] = {};
                        cgltf_accessor_read_float(position, v, p3, 3);
                        for (int j = 0; j < 3; j++)
                            model->positions.push_back(m[j] * p3[0] + m[4 + j] * p3[1] + m[8 + j] * p3[2] + m[12 + j]);
                        if (normal != nullptr)
                            cgltf_accessor_read_float(normal, v, n3, 3);
                        float transformed[3], length = 0.0f;
                        for (int j = 0; j < 3; j++)
                        {
                            transformed[j] = m[j] * n3[0] + m[4 + j] * n3[1] + m[8 + j] * n3[2];
                            length += transformed[j] * transformed[j];
                        }
                        length = length > 0.0f ? 1.0f / std::sqrt(length) : 0.0f;
                        for (int j = 0; j < 3; j++)
                            model->normals.push_back(transformed[j] * length);
                        if (texcoord != nullptr)
                            cgltf_accessor_read_float(texcoord, v, uv, 2);
                        model->texcoords.push_back(uv[0]);
                        model->texcoords.push_back(uv[1]);
                        vertexTiles.push_back(tile);
                    }
                    anyNormals |= normal != nullptr;

                    if (primitive.indices != nullptr)
                    {
                        for (cgltf_size i = 0; i < primitive.indices->count; i++)
                            model->indices.push_back(first + uint32_t(cgltf_accessor_read_index(primitive.indices, i)));
                    }
                    else
                    {
                        for (cgltf_size i = 0; i < position->count; i++)
                            model->indices.push_back(first + uint32_t(i));
                    }
                }
            }

            if (model->indices.empty())
            {
                LOGFN_ERROR("Mods: {} has no triangles", file);
                return nullptr;
            }
            if (!anyNormals)
                model->normals.clear();
            BuildAtlas(*model, tiles, vertexTiles);
            model->brightLuminance = ImageBrightness(model->texture);
            return model;
        }

        // The title, author and license Sketchfab and other tools put in a model's asset.extras.
        std::map<std::string, std::string> ReadModelCredits(const std::filesystem::path& path)
        {
            std::map<std::string, std::string> credits;
            cgltf_options options{};
            cgltf_data* data = nullptr;
            if (cgltf_parse_file(&options, path.string().c_str(), &data) != cgltf_result_success)
                return credits;
            cgltf_size size = 0;
            if (cgltf_copy_extras_json(data, &data->asset.extras, nullptr, &size) == cgltf_result_success && size > 1)
            {
                std::string json(size, '\0');
                cgltf_copy_extras_json(data, &data->asset.extras, json.data(), &size);
                for (const char* key : { "title", "author", "license" })
                {
                    // "key": "value", without escapes; enough for display.
                    auto at = json.find(fmt::format("\"{}\"", key));
                    if (at == std::string::npos)
                        continue;
                    auto open = json.find('"', json.find(':', at) + 1);
                    auto close = open == std::string::npos ? open : json.find('"', open + 1);
                    if (close != std::string::npos)
                        credits[key] = json.substr(open + 1, close - open - 1);
                }
            }
            cgltf_free(data);
            return credits;
        }

        // Drops links in brackets ("name (https://...)"), which make credits hard to read.
        std::string WithoutLinks(std::string text)
        {
            for (size_t at; (at = text.find(" (http")) != std::string::npos;)
            {
                size_t close = text.find(')', at);
                text.erase(at, close == std::string::npos ? std::string::npos : close - at + 1);
            }
            return text;
        }

        std::string FormatCredits(std::string author, std::string license)
        {
            author = WithoutLinks(author);
            license = WithoutLinks(license);
            if (author.empty())
                return license;
            return license.empty() ? fmt::format("by {}", author) : fmt::format("by {} ({})", author, license);
        }

        // ---- Loading ----

        void ReadVector(const toml::node* node, float out[3])
        {
            if (const toml::array* array = node != nullptr ? node->as_array() : nullptr)
            {
                for (size_t i = 0; i < 3 && i < array->size(); i++)
                    out[i] = (*array)[i].value<double>().value_or(0.0);
            }
        }

        // The keys of a replacement, at the top of a model mod's mod.toml or in a [[mesh]].
        struct ReplacementKeys
        {
            std::string model;
            Transform transform;
        };

        ReplacementKeys ReadReplacement(const toml::table& table, const std::string& mod, bool fitByDefault, MeshReplacement& replacement)
        {
            ReplacementKeys keys;
            replacement.mod = mod;
            replacement.hide = table["hide"].value_or(false);
            replacement.fitBounds = table["fit"].value_or(std::string(fitByDefault ? "bounds" : "none")) == "bounds";
            replacement.textures.fill(TextureSource::Auto);
            replacement.matchBrightness = true;
            replacement.brightness = float(table["brightness"].value_or(1.0));
            if (auto slot = table["texture_slot"].value<int64_t>(); slot && *slot >= 0 && *slot < 16)
                replacement.textures[*slot] = TextureSource::Model;
            if (const toml::table* textures = table["textures"].as_table())
            {
                static const std::pair<const char*, TextureSource> SOURCES[] = { { "auto", TextureSource::Auto }, { "game", TextureSource::Game },
                    { "model", TextureSource::Model }, { "flat_normal", TextureSource::FlatNormal }, { "black", TextureSource::Black },
                    { "white", TextureSource::White } };
                for (const auto& [key, value] : *textures)
                {
                    int slot = -1;
                    std::from_chars(key.str().data(), key.str().data() + key.str().size(), slot);
                    std::string source = value.value_or(std::string());
                    auto it = std::find_if(std::begin(SOURCES), std::end(SOURCES), [&](const auto& s) { return source == s.first; });
                    if (slot < 0 || slot >= 16 || it == std::end(SOURCES))
                        LOGFN_ERROR("Mods: {}: textures: \"{}\" = \"{}\" isn't a slot (0-15) and one of auto, game, model, flat_normal, black, white",
                            mod, key.str(), source);
                    else
                        replacement.textures[slot] = it->second;
                }
            }
            keys.model = table["model"].value_or(std::string());
            keys.transform.scale = float(table["scale"].value_or(1.0));
            ReadVector(table.get("rotate"), keys.transform.rotate);
            ReadVector(table.get("offset"), keys.transform.offset);
            return keys;
        }

        std::shared_ptr<const Model> LoadAndLog(const std::string& mod, const std::filesystem::path& path, const Transform& transform)
        {
            auto model = LoadModel(path);
            if (!model)
                return nullptr;
            TransformModel(*model, transform);
            LOGFN("Mods: {}: {} ({} vertices, {} triangles, {}x{} texture)", mod, path.filename().string(), model->VertexCount(),
                model->indices.size() / 3, model->textureWidth, model->textureHeight);
            return model;
        }

        bool IsModelMod(const ModSource& source, const toml::table* manifest)
        {
            return source.manifest.empty() || (manifest != nullptr && !(*manifest)["mesh"].as_array());
        }

        // Every installed mod, with the character each model mod replaces: the mods menu's
        // choices, else the character the mod asks for if no other mod has it.
        std::vector<ModInfo> Resolve(const std::filesystem::path& directory, std::vector<ModSource>& sources)
        {
            Settings settings = LoadSettings(directory);
            sources = FindMods(directory);
            std::vector<ModInfo> list(sources.size());
            std::vector<std::string> wanted(sources.size());
            const auto& characters = Characters();
            auto known = [&](const std::string& package)
                { return std::any_of(characters.begin(), characters.end(), [&](const Character& c) { return c.package == package; }); };

            for (size_t i = 0; i < sources.size(); i++)
            {
                const ModSource& source = sources[i];
                ModInfo& info = list[i];
                info.id = source.id;
                info.enabled = !settings.off.contains(source.id);
                std::optional<toml::table> manifest;
                if (!source.manifest.empty())
                    manifest = ReadManifest(source.manifest);
                info.isModel = IsModelMod(source, manifest ? &*manifest : nullptr);
                if (manifest)
                {
                    info.name = (*manifest)["name"].value_or(source.id);
                    info.credits = FormatCredits((*manifest)["author"].value_or(std::string()), (*manifest)["license"].value_or(std::string()));
                    wanted[i] = (*manifest)["character"].value_or(std::string(DEFAULT_CHARACTER));
                    if (!info.isModel)
                    {
                        size_t count = 0;
                        for (const toml::node& mesh : *(*manifest)["mesh"].as_array())
                            if (const toml::table* table = mesh.as_table())
                                count += (*table)["replace"].is_array() ? (*table)["replace"].as_array()->size() : 1;
                        info.meshes = fmt::format("{} mesh{}", count, count == 1 ? "" : "es");
                    }
                }
                else if (!source.manifest.empty())
                {
                    info.name = source.id;
                    info.isModel = false;
                    info.enabled = false;
                    info.meshes = "mod.toml has an error";
                }
                else
                {
                    auto credits = ReadModelCredits(source.model);
                    info.name = credits.contains("title") ? credits["title"] : source.model.stem().string();
                    info.credits = FormatCredits(credits["author"], credits["license"]);
                    wanted[i] = DEFAULT_CHARACTER;
                }
            }

            // The menu's choices first, then each remaining model mod gets the character it asks
            // for if that's free.
            std::set<std::string> taken;
            for (const auto& [character, mod] : settings.characters)
            {
                for (size_t i = 0; i < list.size(); i++)
                    if (list[i].id == mod && list[i].isModel && list[i].character.empty() && known(character) && !taken.contains(character))
                    {
                        list[i].character = character;
                        taken.insert(character);
                    }
            }
            for (size_t i = 0; i < list.size(); i++)
            {
                ModInfo& info = list[i];
                bool chosen = std::any_of(settings.characters.begin(), settings.characters.end(), [&](const auto& c) { return c.second == info.id; });
                if (!info.isModel || !info.character.empty() || chosen || settings.off.contains(info.id))
                    continue;
                if (known(wanted[i]) && !taken.contains(wanted[i]))
                {
                    info.character = wanted[i];
                    taken.insert(wanted[i]);
                }
            }
            return list;
        }

        void LoadModelMod(const ModSource& source, const ModInfo& info)
        {
            MeshReplacement replacement;
            ReplacementKeys keys;
            std::filesystem::path model = source.model;
            if (!source.manifest.empty())
            {
                auto manifest = ReadManifest(source.manifest);
                if (!manifest)
                    return;
                keys = ReadReplacement(*manifest, info.name, true, replacement);
                if (keys.model.empty())
                {
                    LOGFN_ERROR("Mods: {}: mod.toml needs model = \"file.glb\"", info.name);
                    return;
                }
                model = source.directory / keys.model;
            }
            else
            {
                ReadReplacement(toml::table{}, info.name, true, replacement);
            }
            replacement.model = LoadAndLog(info.name, model, keys.transform);
            if (!replacement.model)
                return;
            g_characters[info.character] = std::move(replacement);
            auto character = std::find_if(Characters().begin(), Characters().end(), [&](const Character& c) { return c.package == info.character; });
            LOGFN("Mods: \"{}\" replaces {}", info.name, character != Characters().end() ? character->label : info.character);
        }

        void LoadMeshMod(const ModSource& source, const ModInfo& info)
        {
            auto manifest = ReadManifest(source.manifest);
            if (!manifest)
                return;
            uint32_t replaced = 0;
            std::map<std::string, std::shared_ptr<const Model>> models;
            for (const toml::node& node : *(*manifest)["mesh"].as_array())
            {
                const toml::table* mesh = node.as_table();
                if (mesh == nullptr)
                    continue;

                std::vector<uint64_t> ids;
                auto readId = [&](const toml::node& id)
                    {
                        if (auto text = id.value<std::string>())
                        {
                            uint64_t value = 0;
                            auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), value, 16);
                            if (error == std::errc() && end == text->data() + text->size())
                                ids.push_back(value);
                            else
                                LOGFN_ERROR("Mods: {}: \"{}\" is not a mesh ID", info.name, *text);
                        }
                    };
                if (const toml::array* list = (*mesh)["replace"].as_array())
                    for (const toml::node& id : *list)
                        readId(id);
                else if (const toml::node* id = mesh->get("replace"))
                    readId(*id);
                if (ids.empty())
                {
                    LOGFN_ERROR("Mods: {}: a [[mesh]] needs replace = \"<mesh ID>\"", info.name);
                    continue;
                }

                MeshReplacement replacement;
                ReplacementKeys keys = ReadReplacement(*mesh, info.name, false, replacement);
                if (!replacement.hide)
                {
                    if (keys.model.empty())
                    {
                        LOGFN_ERROR("Mods: {}: a [[mesh]] needs model = \"file.glb\" or hide = true", info.name);
                        continue;
                    }
                    const Transform& t = keys.transform;
                    std::string key = fmt::format("{}|{}|{}|{}|{}|{}|{}|{}", keys.model, t.scale, t.rotate[0], t.rotate[1], t.rotate[2], t.offset[0],
                        t.offset[1], t.offset[2]);
                    auto& model = models[key];
                    if (!model)
                        model = LoadAndLog(info.name, source.directory / keys.model, t);
                    if (!model)
                        continue;
                    replacement.model = model;
                }
                for (uint64_t id : ids)
                {
                    if (auto it = g_meshes.find(id); it != g_meshes.end())
                        LOGFN_WARNING("Mods: {} and {} both replace mesh {:016x}; using {}", it->second.mod, info.name, id, info.name);
                    g_meshes[id] = replacement;
                }
                replaced += uint32_t(ids.size());
            }
            LOGFN("Mods: loaded \"{}\" ({} meshes)", info.name, replaced);
        }
    }

    float BrightLuminance(const std::vector<uint8_t>& rgba)
    {
        return ImageBrightness(rgba);
    }

    const std::vector<Character>& Characters()
    {
        // The characters the game's data has (all of them if the game isn't found yet).
        static const std::vector<Character> characters = []
            {
                std::vector<Character> present;
                std::error_code ec;
                for (const Character& character : CHARACTERS)
                    if (GetGamePath().empty() || std::filesystem::exists(GetGamePath() / "lu" / (character.package + ".lu"), ec))
                        present.push_back(character);
                LOGFN("Mods: {} characters in the game's data", present.size());
                return present;
            }();
        return characters;
    }

    std::vector<ModInfo> List(const std::filesystem::path& directory)
    {
        std::vector<ModSource> sources;
        return Resolve(directory, sources);
    }

    bool ShowMenu(const std::filesystem::path& directory)
    {
        return LoadSettings(directory).showMenu;
    }

    void SaveSettings(const std::filesystem::path& directory, const std::vector<ModInfo>& mods, bool showMenu)
    {
        toml::array off;
        toml::table characters;
        for (const ModInfo& mod : mods)
        {
            if (mod.isModel && !mod.character.empty())
                characters.insert_or_assign(mod.character, mod.id);
            else if (mod.isModel || !mod.enabled)
                off.push_back(mod.id);
        }
        toml::table table{ { "show_menu", showMenu }, { "off", off }, { "characters", characters } };
        std::ofstream file(directory / "settings.toml");
        file << "# Written by the mods menu.\n" << table << "\n";
    }

    void Init(const std::filesystem::path& directory)
    {
        // NB_NO_MODS=1 starts without them (for comparisons and troubleshooting).
        if (getenv("NB_NO_MODS") != nullptr)
            return;
        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec))
            return;
        std::vector<ModSource> sources;
        std::vector<ModInfo> list = Resolve(directory, sources);
        for (size_t i = 0; i < list.size(); i++)
        {
            if (list[i].isModel && !list[i].character.empty())
                LoadModelMod(sources[i], list[i]);
            else if (!list[i].isModel && list[i].enabled)
                LoadMeshMod(sources[i], list[i]);
            else
                LOGFN("Mods: \"{}\" is off", list[i].name);
        }
    }

    bool Active()
    {
        return !g_meshes.empty() || !g_characters.empty();
    }

    bool ReplacesCharacters()
    {
        return !g_characters.empty();
    }

    const MeshReplacement* FindMesh(uint64_t id)
    {
        auto it = g_meshes.find(id);
        return it != g_meshes.end() ? &it->second : nullptr;
    }

    const MeshReplacement* FindCharacter(const std::string& package)
    {
        auto it = g_characters.find(package);
        return it != g_characters.end() ? &it->second : nullptr;
    }

    bool WriteGlb(const std::filesystem::path& path, const std::vector<float>& positions, const std::vector<float>& normals,
        const std::vector<float>& texcoords, const std::vector<uint32_t>& indices)
    {
        uint32_t vertexCount = uint32_t(positions.size() / 3);
        float minimum[3] = { INFINITY, INFINITY, INFINITY }, maximum[3] = { -INFINITY, -INFINITY, -INFINITY };
        for (uint32_t v = 0; v < vertexCount; v++)
        {
            for (int j = 0; j < 3; j++)
            {
                minimum[j] = std::min(minimum[j], positions[v * 3 + j]);
                maximum[j] = std::max(maximum[j], positions[v * 3 + j]);
            }
        }

        // Binary chunk: positions, normals, texcoords, indices; one buffer view and accessor each.
        std::vector<uint8_t> binary;
        std::string views, accessors, attributes;
        uint32_t accessor = 0;
        auto add = [&](const void* data, size_t size, uint32_t count, const char* type, uint32_t componentType, uint32_t target,
            const std::string& extra)
            {
                size_t offset = binary.size();
                binary.insert(binary.end(), (const uint8_t*)data, (const uint8_t*)data + size);
                binary.resize((binary.size() + 3) & ~size_t(3));
                views += fmt::format("{}{{\"buffer\":0,\"byteOffset\":{},\"byteLength\":{},\"target\":{}}}", views.empty() ? "" : ",", offset, size, target);
                accessors += fmt::format("{}{{\"bufferView\":{},\"componentType\":{},\"count\":{},\"type\":\"{}\"{}}}", accessors.empty() ? "" : ",",
                    accessor, componentType, count, type, extra);
                return accessor++;
            };
        constexpr uint32_t FLOAT = 5126, UNSIGNED_INT = 5125, ARRAY_BUFFER = 34962, ELEMENT_ARRAY_BUFFER = 34963;
        attributes += fmt::format("\"POSITION\":{}", add(positions.data(), positions.size() * 4, vertexCount, "VEC3", FLOAT, ARRAY_BUFFER,
            fmt::format(",\"min\":[{},{},{}],\"max\":[{},{},{}]", minimum[0], minimum[1], minimum[2], maximum[0], maximum[1], maximum[2])));
        if (!normals.empty())
            attributes += fmt::format(",\"NORMAL\":{}", add(normals.data(), normals.size() * 4, vertexCount, "VEC3", FLOAT, ARRAY_BUFFER, ""));
        if (!texcoords.empty())
            attributes += fmt::format(",\"TEXCOORD_0\":{}", add(texcoords.data(), texcoords.size() * 4, vertexCount, "VEC2", FLOAT, ARRAY_BUFFER, ""));
        uint32_t indexAccessor = add(indices.data(), indices.size() * 4, uint32_t(indices.size()), "SCALAR", UNSIGNED_INT, ELEMENT_ARRAY_BUFFER, "");

        std::string json = fmt::format(
            "{{\"asset\":{{\"version\":\"2.0\",\"generator\":\"NaughtyBearRecomp\"}},\"scene\":0,\"scenes\":[{{\"nodes\":[0]}}],"
            "\"nodes\":[{{\"mesh\":0,\"name\":\"{}\"}}],\"meshes\":[{{\"primitives\":[{{\"attributes\":{{{}}},\"indices\":{}}}]}}],"
            "\"buffers\":[{{\"byteLength\":{}}}],\"bufferViews\":[{}],\"accessors\":[{}]}}",
            path.stem().string(), attributes, indexAccessor, binary.size(), views, accessors);
        json.resize((json.size() + 3) & ~size_t(3), ' ');

        std::ofstream file(path, std::ios::binary);
        auto write32 = [&](uint32_t value) { file.write((const char*)&value, 4); };
        write32(0x46546C67); // "glTF"
        write32(2);
        write32(uint32_t(12 + 8 + json.size() + 8 + binary.size()));
        write32(uint32_t(json.size()));
        write32(0x4E4F534A); // "JSON"
        file.write(json.data(), json.size());
        write32(uint32_t(binary.size()));
        write32(0x004E4942); // "BIN"
        file.write((const char*)binary.data(), binary.size());
        return bool(file);
    }
}

#endif
