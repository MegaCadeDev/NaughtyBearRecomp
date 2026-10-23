#include <stdafx.h>
#include "mods.h"
#include <os/logger.h>
#include <cgltf.h>
#include <stb_image.h>
#include <toml.hpp>
#include <ankerl/unordered_dense.h>
#include <cmath>
#include <map>

// A mod is any of these in the mods folder:
//
//   - a model file (.glb or .gltf): it replaces Naughty Bear;
//   - a folder with a model file and no mod.toml: the same;
//   - a folder with a mod.toml:
//
//       name = "My bear"
//       author = "..."                 # shown in the mods menu
//       license = "CC-BY-4.0"
//
//       [[mesh]]
//       target = "naughty_bear"        # a named target (TARGETS below), or:
//       replace = "0123456789abcdef"   # mesh ID(s) from NB_MOD_EXPORT; a string or a list
//       model = "bear.glb"             # .glb or .gltf in the mod's folder
//       texture_slot = 4               # the game texture slot the model's colours go in, or:
//       textures = { 4 = "model", 1 = "flat_normal", 2 = "black" }
//                                      # per slot: model, flat_normal, black, white or game
//                                      # (a target sets them all)
//       fit = "bounds"                 # fit the model onto the game mesh ("none" with replace)
//       brightness = 1.0               # multiplies the model's colours (a target scales them
//                                      # to suit its shader first)
//       scale = 1.0                    # applied to the model first, then
//       rotate = [0, 0, 0]             # degrees about X, Y, Z, then
//       offset = [0, 0, 0]             # a translation
//
//       [[mesh]]
//       replace = "fedcba9876543210"
//       hide = true                    # draw nothing for this mesh
//
// mods/settings.toml, written by the mods menu, lists the disabled mods. Folders whose name
// starts with "_" or "." (such as _export) are not mods.
namespace mods
{
    namespace
    {
        // Game meshes known by name, so mods don't need mesh IDs.
        struct Target
        {
            const char* key;
            const char* label;
            std::vector<uint64_t> meshes;
            // What the model puts in each texture slot; the rest keep the game's.
            std::vector<std::pair<uint32_t, TextureSource>> textures;
            // The game's shader for the mesh is made for its own colours; models' colours are
            // scaled to a similar brightness.
            float brightness;
        };

        // Naughty Bear's textures: 0 the lighting ramp and 3 fur noise (neither follows the
        // model's layout), 1 the normal map, 2 a glow mask (the eyes), 4 the colours, 5 effect
        // masks, 6 a tinted copy of the colours (the fur's sheen). His fur colours are dark and
        // the shader lights them brightly: his brightest are about 60% of white.
        const Target TARGETS[] =
        {
            { "naughty_bear", "Naughty Bear", { 0xA62EF2EBBDE0F1ABull },
                { { 1, TextureSource::FlatNormal }, { 2, TextureSource::Black }, { 4, TextureSource::Model }, { 5, TextureSource::Black },
                    { 6, TextureSource::Model } }, 0.6f },
        };
        constexpr const char* DEFAULT_TARGET = "naughty_bear";

        const Target* FindTarget(const std::string& key)
        {
            for (const Target& target : TARGETS)
                if (key == target.key)
                    return &target;
            return nullptr;
        }

        ankerl::unordered_dense::map<uint64_t, MeshReplacement> g_meshes;

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
            std::filesystem::path directory;  // where its files are
            std::filesystem::path manifest;   // mod.toml, or empty
            std::filesystem::path model;      // the model, without a manifest
        };

        std::vector<ModSource> FindMods(const std::filesystem::path& directory)
        {
            std::vector<ModSource> sources;
            std::error_code ec;
            std::vector<std::filesystem::path> entries;
            for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
                entries.push_back(entry.path());
            // Mods load in name order; a later mod wins when two replace the same mesh.
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

        // ---- Settings ----

        struct Settings
        {
            bool showMenu = true;
            std::set<std::string> disabled;
        };

        Settings LoadSettings(const std::filesystem::path& directory)
        {
            Settings settings;
            auto path = directory / "settings.toml";
            if (!std::filesystem::exists(path))
                return settings;
            try
            {
                toml::table table = toml::parse_file(path.string());
                settings.showMenu = table["show_menu"].value_or(true);
                if (const toml::array* disabled = table["disabled"].as_array())
                    for (const toml::node& id : *disabled)
                        if (auto text = id.value<std::string>())
                            settings.disabled.insert(*text);
            }
            catch (const toml::parse_error& error)
            {
                LOGFN_ERROR("Mods: {}: {}", path.string(), error.description());
            }
            return settings;
        }

        // ---- Models ----

        struct Transform
        {
            float brightness = 1.0f;
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
            if (transform.brightness != 1.0f)
            {
                for (size_t i = 0; i < model.texture.size(); i++)
                    if ((i & 3) != 3)
                        model.texture[i] = uint8_t(std::clamp(float(model.texture[i]) * transform.brightness + 0.5f, 0.0f, 255.0f));
            }
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

        void ApplyTarget(const Target& target, MeshReplacement& replacement)
        {
            replacement.textures.fill(TextureSource::Game);
            for (auto [slot, source] : target.textures)
                replacement.textures[slot] = source;
        }

        void Replace(const std::vector<uint64_t>& ids, const MeshReplacement& replacement)
        {
            for (uint64_t id : ids)
            {
                if (auto it = g_meshes.find(id); it != g_meshes.end())
                    LOGFN_WARNING("Mods: {} and {} both replace mesh {:016x}; using {}", it->second.mod, replacement.mod, id, replacement.mod);
                g_meshes[id] = replacement;
            }
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

        // A model on its own: it replaces the default target, fitted onto it.
        void LoadModelMod(const ModSource& source)
        {
            const Target* target = FindTarget(DEFAULT_TARGET);
            MeshReplacement replacement;
            replacement.mod = source.id;
            replacement.fitBounds = true;
            ApplyTarget(*target, replacement);
            Transform transform;
            transform.brightness = target->brightness;
            replacement.model = LoadAndLog(source.id, source.model, transform);
            if (!replacement.model)
                return;
            Replace(target->meshes, replacement);
            LOGFN("Mods: loaded \"{}\" (replaces {})", source.id, target->label);
        }

        void ReadVector(const toml::node* node, float out[3])
        {
            if (const toml::array* array = node != nullptr ? node->as_array() : nullptr)
            {
                for (size_t i = 0; i < 3 && i < array->size(); i++)
                    out[i] = (*array)[i].value<double>().value_or(0.0);
            }
        }

        void LoadManifestMod(const ModSource& source)
        {
            toml::table manifest;
            try
            {
                manifest = toml::parse_file(source.manifest.string());
            }
            catch (const toml::parse_error& error)
            {
                LOGFN_ERROR("Mods: {}: {}", source.manifest.string(), error.description());
                return;
            }

            std::string name = manifest["name"].value_or(source.id);
            uint32_t replaced = 0;
            std::map<std::string, std::shared_ptr<const Model>> models;
            const toml::array* meshes = manifest["mesh"].as_array();
            for (size_t m = 0; meshes != nullptr && m < meshes->size(); m++)
            {
                const toml::table* mesh = (*meshes)[m].as_table();
                if (mesh == nullptr)
                    continue;

                std::vector<uint64_t> ids;
                const Target* target = nullptr;
                if (auto key = (*mesh)["target"].value<std::string>())
                {
                    target = FindTarget(*key);
                    if (target == nullptr)
                    {
                        LOGFN_ERROR("Mods: {}: unknown target \"{}\"", name, *key);
                        continue;
                    }
                    ids = target->meshes;
                }
                auto readId = [&](const toml::node& node)
                    {
                        if (auto text = node.value<std::string>())
                        {
                            uint64_t id = 0;
                            auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), id, 16);
                            if (error == std::errc() && end == text->data() + text->size())
                                ids.push_back(id);
                            else
                                LOGFN_ERROR("Mods: {}: \"{}\" is not a mesh ID", name, *text);
                        }
                    };
                if (const toml::array* list = (*mesh)["replace"].as_array())
                    for (const toml::node& id : *list)
                        readId(id);
                else if (const toml::node* id = mesh->get("replace"))
                    readId(*id);
                if (ids.empty())
                {
                    LOGFN_ERROR("Mods: {}: a [[mesh]] needs target = \"naughty_bear\" or replace = \"<mesh ID>\"", name);
                    continue;
                }

                MeshReplacement replacement;
                replacement.mod = name;
                replacement.hide = (*mesh)["hide"].value_or(false);
                replacement.fitBounds = (*mesh)["fit"].value_or(std::string(target != nullptr ? "bounds" : "none")) == "bounds";
                if (target != nullptr)
                    ApplyTarget(*target, replacement);
                if (auto slot = (*mesh)["texture_slot"].value<int64_t>(); slot && *slot >= 0 && *slot < 16)
                    replacement.textures[*slot] = TextureSource::Model;
                if (const toml::table* textures = (*mesh)["textures"].as_table())
                {
                    for (const auto& [key, value] : *textures)
                    {
                        int slot = -1;
                        std::from_chars(key.str().data(), key.str().data() + key.str().size(), slot);
                        std::string source = value.value_or(std::string());
                        static const std::pair<const char*, TextureSource> SOURCES[] = { { "game", TextureSource::Game }, { "model", TextureSource::Model },
                            { "flat_normal", TextureSource::FlatNormal }, { "black", TextureSource::Black }, { "white", TextureSource::White } };
                        auto it = std::find_if(std::begin(SOURCES), std::end(SOURCES), [&](const auto& s) { return source == s.first; });
                        if (slot < 0 || slot >= 16 || it == std::end(SOURCES))
                            LOGFN_ERROR("Mods: {}: textures: \"{}\" = \"{}\" isn't a slot (0-15) and one of game, model, flat_normal, black, white",
                                name, key.str(), source);
                        else
                            replacement.textures[slot] = it->second;
                    }
                }
                if (!replacement.hide)
                {
                    std::string file = (*mesh)["model"].value_or(std::string());
                    if (file.empty())
                    {
                        LOGFN_ERROR("Mods: {}: a [[mesh]] needs model = \"file.glb\" or hide = true", name);
                        continue;
                    }
                    Transform transform;
                    transform.brightness = float((*mesh)["brightness"].value_or(1.0)) * (target != nullptr ? target->brightness : 1.0f);
                    transform.scale = float((*mesh)["scale"].value_or(1.0));
                    ReadVector(mesh->get("rotate"), transform.rotate);
                    ReadVector(mesh->get("offset"), transform.offset);
                    std::string key = fmt::format("{}|{}|{}|{}|{}|{}|{}|{}|{}", file, transform.brightness, transform.scale, transform.rotate[0], transform.rotate[1],
                        transform.rotate[2], transform.offset[0], transform.offset[1], transform.offset[2]);
                    auto& model = models[key];
                    if (!model)
                        model = LoadAndLog(name, source.directory / file, transform);
                    if (!model)
                        continue;
                    replacement.model = model;
                }
                Replace(ids, replacement);
                replaced += uint32_t(ids.size());
            }
            LOGFN("Mods: loaded \"{}\" ({} meshes)", name, replaced);
        }
    }

    std::vector<ModInfo> List(const std::filesystem::path& directory)
    {
        Settings settings = LoadSettings(directory);
        std::vector<ModInfo> list;
        for (const ModSource& source : FindMods(directory))
        {
            ModInfo info;
            info.id = source.id;
            info.enabled = !settings.disabled.contains(source.id);
            if (!source.manifest.empty())
            {
                try
                {
                    toml::table manifest = toml::parse_file(source.manifest.string());
                    info.name = manifest["name"].value_or(source.id);
                    info.credits = FormatCredits(manifest["author"].value_or(std::string()), manifest["license"].value_or(std::string()));
                    std::set<std::string> targets;
                    uint32_t meshCount = 0;
                    if (const toml::array* meshes = manifest["mesh"].as_array())
                        for (const toml::node& mesh : *meshes)
                        {
                            const toml::table* table = mesh.as_table();
                            const Target* target = table != nullptr ? FindTarget((*table)["target"].value_or(std::string())) : nullptr;
                            if (target != nullptr)
                                targets.insert(target->label);
                            else
                                meshCount++;
                        }
                    for (const auto& target : targets)
                        info.target += (info.target.empty() ? "" : ", ") + target;
                    if (meshCount != 0)
                        info.target += fmt::format("{}{} mesh{}", info.target.empty() ? "" : ", ", meshCount, meshCount == 1 ? "" : "es");
                }
                catch (const toml::parse_error&)
                {
                    info.name = source.id;
                    info.target = "mod.toml has an error";
                }
            }
            else
            {
                auto credits = ReadModelCredits(source.model);
                info.name = credits.contains("title") ? credits["title"] : source.model.stem().string();
                info.target = FindTarget(DEFAULT_TARGET)->label;
                info.credits = FormatCredits(credits["author"], credits["license"]);
            }
            list.push_back(std::move(info));
        }
        return list;
    }

    bool ShowMenu(const std::filesystem::path& directory)
    {
        return LoadSettings(directory).showMenu;
    }

    void SaveSettings(const std::filesystem::path& directory, const std::vector<ModInfo>& mods, bool showMenu)
    {
        toml::array disabled;
        for (const ModInfo& mod : mods)
            if (!mod.enabled)
                disabled.push_back(mod.id);
        toml::table table{ { "show_menu", showMenu }, { "disabled", disabled } };
        std::ofstream file(directory / "settings.toml");
        file << "# Written by the mods menu.\n" << table << "\n";
    }

    void Init(const std::filesystem::path& directory)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec))
            return;
        Settings settings = LoadSettings(directory);
        for (const ModSource& source : FindMods(directory))
        {
            if (settings.disabled.contains(source.id))
            {
                LOGFN("Mods: \"{}\" is disabled", source.id);
                continue;
            }
            if (!source.manifest.empty())
                LoadManifestMod(source);
            else
                LoadModelMod(source);
        }
    }

    bool Active()
    {
        return !g_meshes.empty();
    }

    const MeshReplacement* FindMesh(uint64_t id)
    {
        auto it = g_meshes.find(id);
        return it != g_meshes.end() ? &it->second : nullptr;
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
