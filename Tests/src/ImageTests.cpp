// Lecture des images (stb_image) et commandes de dessin par texture de la DrawList.
#include "Check.hpp"

#include "Flux/Assets/Image.hpp"
#include "Flux/Graphics/Font.hpp"
#include "Flux/Platform/Filesystem.hpp"
#include "Flux/UI/DrawList.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace Flux;
using Math::Vector2f;

namespace {
    // PNG RGBA 3 x 2 : rouge, vert, bleu ; blanc transparent, noir, gris à moitié transparent.
    constexpr std::array<std::uint8_t, 82> RgbaPng{
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
        0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x9d, 0x74, 0x66, 0x1a, 0x00, 0x00, 0x00,
        0x19, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 0x0c, 0x19, 0xfe, 0x03, 0x49,
        0x20, 0x06, 0x82, 0xff, 0x0d, 0x40, 0x00, 0x00, 0x9c, 0x82, 0x0b, 0xf7, 0x3b, 0xc6, 0xd6, 0x9e, 0x00, 0x00,
        0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

    // PNG en niveaux de gris 2 x 1, sans alpha : noir, puis 200.
    constexpr std::array<std::uint8_t, 68> GrayPng{
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
        0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x08, 0x00, 0x00, 0x00, 0x00, 0xd1, 0x49, 0x20, 0x56, 0x00, 0x00, 0x00,
        0x0b, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x38, 0x01, 0x00, 0x00, 0xcb, 0x00, 0xc9, 0x69, 0xc8,
        0xc3, 0x6c, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

    bool Pixel(const ImageData& image, int x, int y, std::array<std::uint8_t, 4> expected) {
        const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.size.x) + static_cast<std::size_t>(x)) * 4;
        return image.rgba[index] == expected[0] && image.rgba[index + 1] == expected[1] && image.rgba[index + 2] == expected[2]
            && image.rgba[index + 3] == expected[3];
    }

    bool RunLoadTests() {
        auto rgba = LoadImageMemory(RgbaPng);
        CHECK(rgba.has_value());
        CHECK(rgba->size == (Math::Vector2i{3, 2}) && rgba->rgba.size() == 3 * 2 * 4);
        CHECK(Pixel(*rgba, 0, 0, {255, 0, 0, 255}) && Pixel(*rgba, 2, 0, {0, 0, 255, 255}));
        CHECK(Pixel(*rgba, 0, 1, {255, 255, 255, 0}) && Pixel(*rgba, 2, 1, {128, 128, 128, 128})); // alpha tel quel

        // Sans alpha, en gris : converti en RGBA opaque.
        auto gray = LoadImageMemory(GrayPng);
        CHECK(gray.has_value() && gray->size == (Math::Vector2i{2, 1}));
        CHECK(Pixel(*gray, 0, 0, {0, 0, 0, 255}) && Pixel(*gray, 1, 0, {200, 200, 200, 255}));

        // Données invalides, vides ou tronquées : une erreur, pas un plantage.
        const std::array<std::uint8_t, 4> garbage{1, 2, 3, 4};
        CHECK(!LoadImageMemory(garbage).has_value());
        CHECK(!LoadImageMemory({}).has_value());
        CHECK(!LoadImageMemory(std::span(RgbaPng).first(40)).has_value());

        // Largeur annoncée de 20000 pixels : refusée d'après l'en-tête, sans décoder.
        std::array<std::uint8_t, RgbaPng.size()> huge = RgbaPng;
        huge[18] = 0x4e;
        huge[19] = 0x20;
        const auto refused = LoadImageMemory(huge);
        CHECK(!refused.has_value() && refused.error().find("16384") != std::string::npos);

        // Fichier au nom accentué (chemin Unicode sous Windows), puis fichier absent.
        const std::filesystem::path folder = std::filesystem::temp_directory_path() / "FluxTests";
        std::filesystem::create_directories(folder);
        const std::filesystem::path file = folder / Platform::PathFromUtf8("Été.png");
        {
            std::ofstream out(file, std::ios::binary);
            out.write(reinterpret_cast<const char*>(RgbaPng.data()), static_cast<std::streamsize>(RgbaPng.size()));
        }
        const auto fromFile = LoadImageFile(file);
        std::filesystem::remove(file);
        CHECK(fromFile.has_value() && fromFile->size == (Math::Vector2i{3, 2}));
        const auto missing = LoadImageFile(folder / "absent.png");
        CHECK(!missing.has_value() && missing.error().find("absent.png") != std::string::npos);
        return true;
    }

    bool RunCommandTests(const UI::Font& font) {
        UI::DrawList list(font);
        CHECK(list.GetCommands().empty());

        // Rectangles et texte partagent l'atlas : une seule commande, quel que soit leur nombre.
        list.AddRect({0.0f, 0.0f}, {10.0f, 10.0f}, {1.0f, 1.0f, 1.0f});
        list.AddText({0.0f, 0.0f}, "Flux", 16.0f, {1.0f, 1.0f, 1.0f});
        list.AddTriangle({0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f, 1.0f});
        CHECK(list.GetCommands().size() == 1);
        const std::uint32_t atlasVertices = list.GetCommands()[0].vertexCount;
        CHECK(atlasVertices == list.GetVertices().size());

        // Une image, deux fois de suite : une commande ; puis un rectangle : retour à l'atlas.
        list.AddImage({0.0f, 0.0f}, {64.0f, 32.0f}, 3);
        list.AddImage({64.0f, 0.0f}, {128.0f, 32.0f}, 3);
        list.AddRect({0.0f, 0.0f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f});
        CHECK(list.GetCommands().size() == 3);
        const UI::DrawCommand image = list.GetCommands()[1];
        CHECK(image.texture == 3 && image.firstVertex == atlasVertices && image.vertexCount == 12);
        CHECK(list.GetCommands()[2].texture == UI::FontTexture && list.GetCommands()[2].firstVertex == atlasVertices + 12);
        // UV de l'image : toute la texture par défaut.
        CHECK(list.GetVertices()[atlasVertices].uv == (Vector2f{0.0f, 0.0f}));
        CHECK(list.GetVertices()[atlasVertices + 2].uv == (Vector2f{1.0f, 1.0f}));

        // Image découpée de moitié à droite : UV ramenés à 0,5.
        UI::DrawList clipped(font);
        clipped.SetClipRect({0.0f, 0.0f}, {50.0f, 100.0f});
        clipped.AddImage({0.0f, 0.0f}, {100.0f, 100.0f}, 5);
        clipped.AddImage({60.0f, 0.0f}, {100.0f, 100.0f}, 5); // entièrement dehors : rien
        clipped.AddImage({10.0f, 10.0f}, {10.0f, 20.0f}, 5);  // largeur nulle : rien
        CHECK(clipped.GetCommands().size() == 1 && clipped.GetCommands()[0].vertexCount == 6);
        CHECK(clipped.GetVertices()[2].uv == (Vector2f{0.5f, 1.0f}));

        // Append garde les textures, et fusionne avec la commande précédente si elle a la même.
        UI::DrawList merged(font);
        merged.AddImage({0.0f, 0.0f}, {1.0f, 1.0f}, 5);
        merged.Append(clipped);
        merged.Append(list);
        CHECK(merged.GetVertices().size() == 6 + 6 + list.GetVertices().size());
        CHECK(merged.GetCommands().size() == 1 + 3);
        CHECK(merged.GetCommands()[0].texture == 5 && merged.GetCommands()[0].vertexCount == 12);
        CHECK(merged.GetCommands()[1].firstVertex == 12 && merged.GetCommands()[2].texture == 3);

        list.Clear();
        CHECK(list.GetCommands().empty() && list.GetVertices().empty());
        return true;
    }
}

bool RunImageTests() {
    auto font = UI::Font::Load(Platform::GetExecutableDirectory() / "assets" / "fonts" / "Arimo.ttf");
    if (!font) {
        std::printf("ÉCHEC : police introuvable (%s)\n", font.error().c_str());
        return false;
    }
    return RunLoadTests() && RunCommandTests(**font);
}
