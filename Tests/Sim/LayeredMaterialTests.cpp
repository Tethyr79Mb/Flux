// Matériaux multicouches (.fluxmat) : lecture, écriture, et images assemblées au chargement d'un modèle.
#include "Check.hpp"

#include "Flux/Assets/Image.hpp"
#include "Flux/Assets/LayeredMaterial.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace Flux;

namespace {
    void WriteText(const std::filesystem::path& path, std::string_view text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    // Image 2 x 2 d'une seule couleur, écrite en PNG.
    void WriteImage(const std::filesystem::path& path, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        ImageData image;
        image.size = {2, 2};
        for (int i = 0; i < 4; ++i) {
            image.rgba.insert(image.rgba.end(), {r, g, b, 255});
        }
        const std::vector<std::uint8_t> png = EncodePng(image);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    }
}

bool RunLayeredMaterialTests() {
    // Lecture : valeurs par défaut, canal du masque, métal nul sans image de métal.
    constexpr std::string_view Text = R"({
        "format": "flux.material", "version": 1, "mask": "Modeles/Test/usure.png",
        "layers": [
            { "name": "Peinture", "colorImage": "Couches/peinture.png", "tint": [0.5, 0.25, 1.0], "tiling": 8 },
            { "name": "Métal nu", "mask": "g", "normalImage": "Couches/metal-normales.png", "metallicImage": "Couches/metal.png",
              "normalDirectX": true, "sharpness": 8, "heightInfluence": 0.75 }
        ]
    })";
    const auto material = ParseLayeredMaterial(Text);
    CHECK(material.has_value());
    CHECK(material->maskImage == "Modeles/Test/usure.png");
    CHECK(material->layers.size() == 2);
    const LayeredMaterialLayer& paint = material->layers[0];
    CHECK(paint.settings.maskChannel == -1);
    CHECK(paint.settings.metallic == 0.0f);
    CHECK(paint.settings.tiling == 8.0f);
    CHECK(paint.settings.tint.g == 0.25f);
    const LayeredMaterialLayer& metal = material->layers[1];
    CHECK(metal.settings.maskChannel == 1);
    CHECK(metal.settings.metallic == 1.0f);
    CHECK(metal.normalDirectX);
    CHECK(metal.settings.sharpness == 8.0f);

    // Écrit puis relu : le même matériau.
    const auto again = ParseLayeredMaterial(SerializeLayeredMaterial(*material));
    CHECK(again.has_value());
    CHECK(again->maskImage == material->maskImage);
    CHECK(again->layers.size() == 2);
    CHECK(again->layers[1].normalImage == metal.normalImage);
    CHECK(again->layers[1].normalDirectX);
    CHECK(again->layers[1].settings.maskChannel == 1);
    CHECK(again->layers[1].settings.heightInfluence == 0.75f);
    CHECK(again->layers[0].settings.metallic == 0.0f);
    CHECK(again->layers[0].settings.tint.r == 0.5f);

    // Refusés : trop de couches, canal inconnu, répétition nulle bornée.
    CHECK(!ParseLayeredMaterial(R"({"layers": [{}, {}, {}, {}, {}]})").has_value());
    CHECK(!ParseLayeredMaterial(R"({"layers": [{"mask": "x"}]})").has_value());
    CHECK(!ParseLayeredMaterial(R"({"layers": 3})").has_value());
    CHECK(ParseLayeredMaterial(R"({"layers": [{"tiling": 0}]})")->layers[0].settings.tiling > 0.0f);

    // Liste d'un modèle : Raie.glb -> Raie.materiaux.json.
    CHECK(GetMaterialListPath("Modeles/Raie/Raie.glb").filename() == "Raie.materiaux.json");
    const auto list = ParseMaterialList(R"({"materials": {"Coque": "Materiaux/Coque.fluxmat"}})");
    CHECK(list.has_value() && list->size() == 1 && (*list)[0].first == "Coque");
    CHECK(!ParseMaterialList(R"({"materials": {"Coque": 3}})").has_value());

    // Chargement : couches posées sur le matériau nommé, images assemblées en deux par couche.
    const std::filesystem::path assets = std::filesystem::temp_directory_path() / "FluxLayeredMaterialTests";
    std::filesystem::remove_all(assets);
    WriteText(assets / "Modeles/Test/Test.materiaux.json", R"({"materials": {"Coque": "Materiaux/Coque.fluxmat", "Absent": "x.fluxmat"}})");
    WriteText(assets / "Materiaux/Coque.fluxmat", Text);
    WriteImage(assets / "Modeles/Test/usure.png", 0, 200, 0);
    WriteImage(assets / "Couches/peinture.png", 10, 20, 30);
    WriteImage(assets / "Couches/metal-normales.png", 128, 40, 255);
    WriteImage(assets / "Couches/metal.png", 220, 0, 0);

    ModelData data;
    data.materials.resize(2);
    data.materials[0].name = "Verre";
    data.materials[1].name = "Coque";
    std::vector<std::filesystem::path> dependencies;
    ApplyLayeredMaterials(data, assets / "Modeles/Test/Test.glb", assets, dependencies);
    CHECK(data.materials[0].layers.empty());
    const ModelMaterial& hull = data.materials[1];
    CHECK(hull.layers.size() == 2);
    CHECK(hull.layerMaskImage >= 0);
    CHECK(data.images[static_cast<std::size_t>(hull.layerMaskImage)].rgba[1] == 200);
    // Peinture : couleur sans hauteur (alpha neutre, bord qui ne suit que le masque), pas d'image de données.
    const ModelMaterialLayer& paintLayer = hull.layers[0];
    CHECK(paintLayer.colorImage >= 0 && paintLayer.dataImage == -1);
    const std::vector<std::uint8_t>& color = data.images[static_cast<std::size_t>(paintLayer.colorImage)].rgba;
    CHECK(color[0] == 10 && color[2] == 30 && color[3] == 128);
    CHECK(paintLayer.heightInfluence == 0.0f);
    // Métal : normale DirectX retournée (vert 40 -> 215), rugosité pleine sans image, métal du canal rouge.
    const ModelMaterialLayer& metalLayer = hull.layers[1];
    CHECK(metalLayer.colorImage == -1 && metalLayer.dataImage >= 0);
    const std::vector<std::uint8_t>& packed = data.images[static_cast<std::size_t>(metalLayer.dataImage)].rgba;
    CHECK(packed[0] == 128 && packed[1] == 215 && packed[2] == 255 && packed[3] == 220);
    CHECK(metalLayer.normalScale == 1.0f);
    // Suivis : la liste, le matériau, ses images (relu si l'un change).
    const auto follows = [&](const std::filesystem::path& path) { return std::ranges::find(dependencies, path) != dependencies.end(); };
    CHECK(follows(assets / "Modeles/Test/Test.materiaux.json"));
    CHECK(follows(assets / "Materiaux/Coque.fluxmat"));
    CHECK(follows(assets / "Couches/metal.png"));

    // Sans liste : rien de changé, la liste suivie quand même (son apparition relit le modèle).
    ModelData plain;
    plain.materials.resize(1);
    dependencies.clear();
    ApplyLayeredMaterials(plain, assets / "Modeles/Test/Autre.glb", assets, dependencies);
    CHECK(plain.materials[0].layers.empty() && plain.images.empty());
    CHECK(dependencies.size() == 1);
    std::filesystem::remove_all(assets);
    return true;
}
