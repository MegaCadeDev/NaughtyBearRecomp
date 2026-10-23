#include <stdafx.h>
#include "mods.h"
#include <os/logger.h>
#include <cgltf.h>
#include <stb_image.h>
#include <toml.hpp>
#include <ankerl/unordered_dense.h>
#include <cmath>
#include <map>

// mods/<folder>/mod.toml:
//
//   name = "My bear"
//
//   [[mesh]]
//   replace = "0123456789abcdef"   # mesh ID(s) from NB_MOD_EXPORT; a string or a list
//   model = "bear.glb"             # .glb or .gltf in the mod's folder
//   texture_slot = 0               # game texture the model's texture replaces (-1: keep)
//   fit = "none"                   # or "bounds": fit the model onto the game mesh
//   scale = 1.0                    # applied to the model first, then
//   rotate = [0, 0, 0]             # degrees about X, Y, Z, then
//   offset = [0, 0, 0]             # a translation
//
//   [[mesh]]
//   replace = "fedcba9876543210"
//   hide = true                    # draw nothing for this mesh
namespace mods
{
    namespace
    {
        ankerl::unordered_dense::map<uint64_t, MeshReplacement> g_meshes;

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

        bool LoadTexture(const cgltf_image* image, const std::filesystem::path& directory, Model& model)
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
            model.textureWidth = uint32_t(width);
            model.textureHeight = uint32_t(height);
            model.texture.assign(pixels, pixels + size_t(width) * height * 4);
            stbi_image_free(pixels);
            return true;
        }

        // Every triangle primitive of every node, in world space.
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
            bool anyNormals = false, anyTexcoords = false;
            for (cgltf_size n = 0; n < data->nodes_count; n++)
            {
                const cgltf_node* node = &data->nodes[n];
                if (node->mesh == nullptr)
                    continue;
                float world[16];
                cgltf_node_transform_world(node, world);
                for (cgltf_size p = 0; p < node->mesh->primitives_count; p++)
                {
                    const cgltf_primitive& primitive = node->mesh->primitives[p];
                    if (primitive.type != cgltf_primitive_type_triangles)
                        continue;
                    const cgltf_accessor *position = nullptr, *normal = nullptr, *texcoord = nullptr;
                    for (cgltf_size a = 0; a < primitive.attributes_count; a++)
                    {
                        const cgltf_attribute& attribute = primitive.attributes[a];
                        if (attribute.type == cgltf_attribute_type_position)
                            position = attribute.data;
                        else if (attribute.type == cgltf_attribute_type_normal)
                            normal = attribute.data;
                        else if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0)
                            texcoord = attribute.data;
                    }
                    if (position == nullptr)
                        continue;

                    uint32_t first = model->VertexCount();
                    for (cgltf_size v = 0; v < position->count; v++)
                    {
                        float p3[3] = {}, n3[3] = { 0.0f, 0.0f, 1.0f }, uv[2] = {};
                        cgltf_accessor_read_float(position, v, p3, 3);
                        for (int j = 0; j < 3; j++)
                            model->positions.push_back(world[j] * p3[0] + world[4 + j] * p3[1] + world[8 + j] * p3[2] + world[12 + j]);
                        if (normal != nullptr)
                            cgltf_accessor_read_float(normal, v, n3, 3);
                        float transformed[3], length = 0.0f;
                        for (int j = 0; j < 3; j++)
                        {
                            transformed[j] = world[j] * n3[0] + world[4 + j] * n3[1] + world[8 + j] * n3[2];
                            length += transformed[j] * transformed[j];
                        }
                        length = length > 0.0f ? 1.0f / std::sqrt(length) : 0.0f;
                        for (int j = 0; j < 3; j++)
                            model->normals.push_back(transformed[j] * length);
                        if (texcoord != nullptr)
                            cgltf_accessor_read_float(texcoord, v, uv, 2);
                        model->texcoords.push_back(uv[0]);
                        model->texcoords.push_back(uv[1]);
                    }
                    anyNormals |= normal != nullptr;
                    anyTexcoords |= texcoord != nullptr;

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

                    const cgltf_material* material = primitive.material;
                    if (model->texture.empty() && material != nullptr && material->has_pbr_metallic_roughness)
                    {
                        const cgltf_texture* texture = material->pbr_metallic_roughness.base_color_texture.texture;
                        if (texture != nullptr && texture->image != nullptr && !LoadTexture(texture->image, path.parent_path(), *model))
                            LOGFN_WARNING("Mods: could not load the texture of {}", file);
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
            if (!anyTexcoords)
                model->texcoords.clear();
            return model;
        }

        void ReadVector(const toml::node* node, float out[3])
        {
            if (const toml::array* array = node != nullptr ? node->as_array() : nullptr)
            {
                for (size_t i = 0; i < 3 && i < array->size(); i++)
                    out[i] = (*array)[i].value<double>().value_or(0.0);
            }
        }

        void LoadMod(const std::filesystem::path& directory)
        {
            auto manifestPath = directory / "mod.toml";
            std::string folder = directory.filename().string();
            toml::table manifest;
            try
            {
                manifest = toml::parse_file(manifestPath.string());
            }
            catch (const toml::parse_error& error)
            {
                LOGFN_ERROR("Mods: {}: {}", manifestPath.string(), error.description());
                return;
            }

            std::string name = manifest["name"].value_or(folder);
            uint32_t replaced = 0;
            std::map<std::string, std::shared_ptr<const Model>> models;
            if (const toml::array* meshes = manifest["mesh"].as_array())
            {
                for (const toml::node& entry : *meshes)
                {
                    const toml::table* mesh = entry.as_table();
                    if (mesh == nullptr)
                        continue;

                    std::vector<uint64_t> ids;
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
                        LOGFN_ERROR("Mods: {}: a [[mesh]] has no replace = \"<mesh ID>\"", name);
                        continue;
                    }

                    MeshReplacement replacement;
                    replacement.mod = name;
                    replacement.hide = (*mesh)["hide"].value_or(false);
                    replacement.fitBounds = (*mesh)["fit"].value_or(std::string("none")) == "bounds";
                    replacement.textureSlot = int32_t((*mesh)["texture_slot"].value_or(int64_t(0)));
                    if (!replacement.hide)
                    {
                        std::string file = (*mesh)["model"].value_or(std::string());
                        if (file.empty())
                        {
                            LOGFN_ERROR("Mods: {}: a [[mesh]] needs model = \"file.glb\" or hide = true", name);
                            continue;
                        }
                        Transform transform;
                        transform.scale = float((*mesh)["scale"].value_or(1.0));
                        ReadVector(mesh->get("rotate"), transform.rotate);
                        ReadVector(mesh->get("offset"), transform.offset);
                        std::string key = fmt::format("{}|{}|{}|{}|{}|{}|{}|{}", file, transform.scale, transform.rotate[0], transform.rotate[1],
                            transform.rotate[2], transform.offset[0], transform.offset[1], transform.offset[2]);
                        auto& model = models[key];
                        if (!model)
                        {
                            auto loaded = LoadModel(directory / file);
                            if (!loaded)
                                continue;
                            TransformModel(*loaded, transform);
                            LOGFN("Mods: {}: {} ({} vertices, {} triangles{})", name, file, loaded->VertexCount(), loaded->indices.size() / 3,
                                loaded->texture.empty() ? ", no texture" : fmt::format(", {}x{} texture", loaded->textureWidth, loaded->textureHeight));
                            model = std::move(loaded);
                        }
                        replacement.model = model;
                    }

                    for (uint64_t id : ids)
                    {
                        if (auto it = g_meshes.find(id); it != g_meshes.end())
                            LOGFN_WARNING("Mods: {} and {} both replace mesh {:016x}; using {}", it->second.mod, name, id, name);
                        g_meshes[id] = replacement;
                        replaced++;
                    }
                }
            }
            LOGFN("Mods: loaded \"{}\" ({} meshes)", name, replaced);
        }
    }

    void Init(const std::filesystem::path& directory)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec))
            return;
        std::vector<std::filesystem::path> folders;
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
        {
            if (entry.is_directory() && std::filesystem::exists(entry.path() / "mod.toml"))
                folders.push_back(entry.path());
        }
        // Mods load in name order; a later mod wins when two replace the same mesh.
        std::sort(folders.begin(), folders.end());
        for (const auto& folder : folders)
            LoadMod(folder);
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
