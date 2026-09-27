// Lecture des modèles glTF (cgltf) : sommets, transformations des nœuds, matériaux, fichiers .gltf et .glb.
#include "Check.hpp"

#include "Flux/Platform/Filesystem.hpp"
#include "Flux/Renderer/Model.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace Flux;
using Math::Vector3f;

namespace {
    // PNG RGBA 3 x 2 (le même que dans ImageTests).
    constexpr std::array<std::uint8_t, 82> Png{
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
        0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x9d, 0x74, 0x66, 0x1a, 0x00, 0x00, 0x00,
        0x19, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 0x0c, 0x19, 0xfe, 0x03, 0x49,
        0x20, 0x06, 0x82, 0xff, 0x0d, 0x40, 0x00, 0x00, 0x9c, 0x82, 0x0b, 0xf7, 0x3b, 0xc6, 0xd6, 0x9e, 0x00, 0x00,
        0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

    std::string Base64(const std::uint8_t* data, std::size_t size) {
        constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (std::size_t i = 0; i < size; i += 3) {
            const std::uint32_t chunk = (static_cast<std::uint32_t>(data[i]) << 16)
                                      | (i + 1 < size ? static_cast<std::uint32_t>(data[i + 1]) << 8 : 0u)
                                      | (i + 2 < size ? static_cast<std::uint32_t>(data[i + 2]) : 0u);
            out += alphabet[(chunk >> 18) & 63];
            out += alphabet[(chunk >> 12) & 63];
            out += i + 1 < size ? alphabet[(chunk >> 6) & 63] : '=';
            out += i + 2 < size ? alphabet[chunk & 63] : '=';
        }
        return out;
    }

    // Un triangle : positions (0,0,0) (1,0,0) (0,1,0), puis (option) normales +z, puis indices 16 bits 0 1 2.
    std::vector<std::uint8_t> TriangleBuffer(bool withNormals) {
        std::vector<float> floats{0, 0, 0, 1, 0, 0, 0, 1, 0};
        if (withNormals) {
            floats.insert(floats.end(), {0, 0, 1, 0, 0, 1, 0, 0, 1});
        }
        std::vector<std::uint8_t> bytes(floats.size() * sizeof(float));
        std::memcpy(bytes.data(), floats.data(), bytes.size());
        for (const std::uint16_t index : {0, 1, 2}) {
            bytes.push_back(static_cast<std::uint8_t>(index & 0xff));
            bytes.push_back(static_cast<std::uint8_t>(index >> 8));
        }
        bytes.resize((bytes.size() + 3) / 4 * 4, 0);
        return bytes;
    }

    // JSON glTF décrivant TriangleBuffer, avec le matériau 0 si extra déclare des matériaux. Accesseurs : 0 les indices, 1 les positions, 2 les normales.
    // buffer : suite de l'objet du buffer 0 (uri) ; node : propriétés du nœud ; extra : propriétés de premier niveau.
    std::string TriangleJson(bool withNormals, std::size_t bufferSize, const std::string& buffer, const std::string& node,
                             const std::string& extra) {
        const std::string indexView = withNormals ? "2" : "1";
        const std::string indexOffset = withNormals ? "72" : "36";
        std::string attributes = R"("POSITION": 1)";
        std::string accessors = R"({"bufferView": )" + indexView + R"(, "componentType": 5123, "count": 3, "type": "SCALAR"},
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]})";
        std::string views = R"({"buffer": 0, "byteOffset": 0, "byteLength": 36})";
        if (withNormals) {
            attributes += R"(, "NORMAL": 2)";
            accessors += R"(, {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"})";
            views += R"(, {"buffer": 0, "byteOffset": 36, "byteLength": 36})";
        }
        views += R"(, {"buffer": 0, "byteOffset": )" + indexOffset + R"(, "byteLength": 6})";
        // Le matériau 0 seulement s'il existe : un indice sans cible rend le fichier invalide.
        const std::string material = extra.find(R"("materials")") != std::string::npos ? R"(, "material": 0)" : "";
        return R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
            "nodes": [{"mesh": 0)" + node + R"(}],
            "meshes": [{"primitives": [{"attributes": {)" + attributes + R"(}, "indices": 0)" + material + R"(}]}],
            "accessors": [)" + accessors + R"(],
            "bufferViews": [)" + views + R"(],
            "buffers": [{"byteLength": )" + std::to_string(bufferSize) + buffer + R"(}])" + extra + "}";
    }

    bool Near(const Vector3f& a, const Vector3f& b) {
        return std::abs(a.x - b.x) < 1.0e-5f && std::abs(a.y - b.y) < 1.0e-5f && std::abs(a.z - b.z) < 1.0e-5f;
    }

    void WriteFile(const std::filesystem::path& path, const void* data, std::size_t size) {
        std::ofstream file(path, std::ios::binary);
        file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    }

    // .gltf texte, .bin à côté (nom accentué : chemins Unicode), image en data: URI, nœud déplacé et agrandi.
    bool RunTextGltfTests(const std::filesystem::path& directory) {
        const std::vector<std::uint8_t> buffer = TriangleBuffer(true);
        WriteFile(directory / Platform::PathFromUtf8("données é.bin"), buffer.data(), buffer.size());
        const std::string material = R"(, "materials": [{"name": "Peau", "pbrMetallicRoughness": {
                "baseColorFactor": [0.5, 0.25, 1, 1], "baseColorTexture": {"index": 0}},
                "alphaMode": "MASK", "alphaCutoff": 0.3, "doubleSided": true}],
            "textures": [{"source": 0}],
            "images": [{"uri": "data:image/png;base64,)" + Base64(Png.data(), Png.size()) + R"("}])";
        const std::string json = TriangleJson(true, buffer.size(), R"(, "uri": "donn%C3%A9es%20%C3%A9.bin")",
                                              R"(, "translation": [0, 0, 5], "scale": [2, 2, 2])", material);
        const std::filesystem::path path = directory / Platform::PathFromUtf8("modèle.gltf");
        WriteFile(path, json.data(), json.size());

        const auto model = LoadModelFile(path);
        if (!model) {
            std::printf("  %s\n", model.error().c_str());
        }
        CHECK(model);
        CHECK(model->vertices.size() == 3);
        CHECK(model->indices == (std::vector<std::uint32_t>{0, 1, 2}));
        CHECK(Near(model->vertices[0].position, {0, 0, 5}));
        CHECK(Near(model->vertices[1].position, {2, 0, 5}));
        CHECK(Near(model->vertices[2].position, {0, 2, 5}));
        CHECK(Near(model->vertices[1].normal, {0, 0, 1})); // l'échelle ne change pas la normale unitaire
        CHECK(Near(model->bounds.min, {0, 0, 5}) && Near(model->bounds.max, {2, 2, 5}));
        CHECK(model->primitives.size() == 1 && model->primitives[0].indexCount == 3 && model->primitives[0].material == 0);
        CHECK(model->materials.size() == 1);
        const ModelMaterial& skin = model->materials[0];
        CHECK(skin.name == "Peau");
        CHECK(skin.baseColor.r == 0.5f && skin.baseColor.g == 0.25f && skin.baseColor.b == 1.0f);
        CHECK(std::abs(skin.alphaCutoff - 0.3f) < 1.0e-6f && skin.doubleSided);
        CHECK(skin.baseColorImage == 0 && model->images.size() == 1);
        CHECK(model->images[0].size.x == 3 && model->images[0].size.y == 2);
        CHECK(model->warnings.empty());

        // À copier avec le .gltf : le .bin (l'image est intégrée).
        const auto files = ListModelFiles(path);
        CHECK(files && *files == std::vector<std::string>{"données é.bin"});
        return true;
    }

    // .glb (JSON et binaire dans un seul fichier), sans normales ni matériau, avec une symétrie.
    bool RunBinaryGltfTests(const std::filesystem::path& directory) {
        const std::vector<std::uint8_t> buffer = TriangleBuffer(false);
        std::string json = TriangleJson(false, buffer.size(), "", R"(, "scale": [-1, 1, 1])", "");
        json.resize((json.size() + 3) / 4 * 4, ' ');

        std::vector<std::uint8_t> glb;
        const auto put32 = [&glb](std::uint32_t value) {
            for (int i = 0; i < 4; ++i) {
                glb.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
            }
        };
        put32(0x46546C67); // « glTF »
        put32(2);
        put32(static_cast<std::uint32_t>(12 + 8 + json.size() + 8 + buffer.size()));
        put32(static_cast<std::uint32_t>(json.size()));
        put32(0x4E4F534A); // « JSON »
        glb.insert(glb.end(), json.begin(), json.end());
        put32(static_cast<std::uint32_t>(buffer.size()));
        put32(0x004E4942); // « BIN »
        glb.insert(glb.end(), buffer.begin(), buffer.end());
        const std::filesystem::path path = directory / "triangle.glb";
        WriteFile(path, glb.data(), glb.size());

        const auto model = LoadModelFile(path);
        if (!model) {
            std::printf("  %s\n", model.error().c_str());
        }
        CHECK(model);
        CHECK(model->vertices.size() == 3 && model->indices.size() == 3);
        // Miroir en x : le triangle est retourné pour rester dans le sens antihoraire vu de devant...
        const Vector3f a = model->vertices[model->indices[0]].position;
        const Vector3f b = model->vertices[model->indices[1]].position;
        const Vector3f c = model->vertices[model->indices[2]].position;
        CHECK(Near(a, {0, 0, 0}) && Near(b, {0, 1, 0}) && Near(c, {-1, 0, 0}));
        const Vector3f facing = Math::Cross(b - a, c - a);
        CHECK(facing.z > 0.0f);
        // ... et ses normales, calculées faute d'en avoir, pointent du même côté.
        CHECK(Near(model->vertices[0].normal, {0, 0, 1}));
        // Matériau blanc par défaut.
        CHECK(model->materials.size() == 1 && model->materials[0].baseColorImage == -1);
        CHECK(model->materials[0].baseColor.r == 1.0f && model->materials[0].alphaCutoff == 0.0f);
        CHECK(ListModelFiles(path) && ListModelFiles(path)->empty()); // .glb autonome
        return true;
    }

    bool RunErrorTests(const std::filesystem::path& directory) {
        CHECK(!LoadModelFile(directory / "absent.glb"));

        const std::string garbage = "pas un modèle";
        WriteFile(directory / "faux.glb", garbage.data(), garbage.size());
        CHECK(!LoadModelFile(directory / "faux.glb"));

        // Compression Draco exigée : refusée avec un message clair plutôt qu'un modèle vide.
        const std::vector<std::uint8_t> buffer = TriangleBuffer(true);
        const std::string json = TriangleJson(true, buffer.size(), R"(, "uri": "draco.bin")", "",
                                              R"(, "extensionsUsed": ["KHR_draco_mesh_compression"],
                                                 "extensionsRequired": ["KHR_draco_mesh_compression"])");
        WriteFile(directory / "draco.bin", buffer.data(), buffer.size());
        WriteFile(directory / "draco.gltf", json.data(), json.size());
        const auto draco = LoadModelFile(directory / "draco.gltf");
        CHECK(!draco && draco.error().find("KHR_draco_mesh_compression") != std::string::npos);

        // .bin manquant.
        const std::string missing = TriangleJson(true, buffer.size(), R"(, "uri": "introuvable.bin")", "", "");
        WriteFile(directory / "sans-bin.gltf", missing.data(), missing.size());
        CHECK(!LoadModelFile(directory / "sans-bin.gltf"));
        return true;
    }
}

bool RunModelTests() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "FluxModelTests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);
    const bool passed = RunTextGltfTests(directory) && RunBinaryGltfTests(directory) && RunErrorTests(directory);
    std::filesystem::remove_all(directory, error);
    return passed;
}
