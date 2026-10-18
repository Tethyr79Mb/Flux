// Textes et images à l'écran : placement selon l'ancre, le décalage et la taille de la vue ; sauvegarde des composants.
#include "Check.hpp"

#include "Flux/Graphics/Font.hpp"
#include "Flux/Platform/Filesystem.hpp"
#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/SceneSerializer.hpp"
#include "Flux/Scene/ScreenUi.hpp"
#include "Flux/UI/DrawList.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Flux;
using Math::Vector2f;

namespace {
    bool Near(Vector2f a, Vector2f b) {
        return std::abs(a.x - b.x) < 0.01f && std::abs(a.y - b.y) < 0.01f;
    }

    bool RunLayoutTests(const UI::Font& font) {
        UI::DrawList list(font);
        Scene scene;
        const Entity title = scene.CreateEntity("Titre");
        scene.Add<ScreenTextComponent>(title, ScreenTextComponent{.text = "Ars Solaris", .size = 64.0f});
        const Entity corner = scene.CreateEntity("Coin");
        scene.Add<ScreenTextComponent>(corner, ScreenTextComponent{.text = "v0.1", .size = 20.0f,
                                                                   .anchor = ScreenAnchor::BottomRight,
                                                                   .offsetX = -10.0f, .offsetY = -10.0f});
        const Entity empty = scene.CreateEntity("Vide", corner); // enfant, texte vide : rien à dessiner
        scene.Add<ScreenTextComponent>(empty, ScreenTextComponent{.text = ""});
        scene.CreateEntity("Cube"); // sans texte

        // Écran de référence (1080 lignes) : tailles telles quelles, texte centré sur son ancre.
        std::vector<ScreenUi::ItemRect> rects;
        ScreenUi::Draw(scene, list, {0.0f, 0.0f}, {1920.0f, 1080.0f}, {.rects = &rects});
        CHECK(rects.size() == 2);
        CHECK(rects[0].entity == title && rects[1].entity == corner); // ordre de la hiérarchie
        const Vector2f titleSize = list.MeasureText("Ars Solaris", 64.0f);
        CHECK(Near(rects[0].min, Vector2f{960.0f, 540.0f} - titleSize * 0.5f));
        CHECK(Near(rects[0].max - rects[0].min, titleSize));
        // En bas à droite : le coin bas-droit du texte sur le coin de l'écran, puis le décalage.
        CHECK(Near(rects[1].max, {1910.0f, 1070.0f}));

        // Vue deux fois plus haute (4K) : tout double, et la vue décalée (éditeur) décale tout.
        rects.clear();
        ScreenUi::Draw(scene, list, {100.0f, 50.0f}, {100.0f + 3840.0f, 50.0f + 2160.0f}, {.rects = &rects});
        CHECK(Near(rects[0].max - rects[0].min, list.MeasureText("Ars Solaris", 128.0f)));
        CHECK(Near(rects[1].max, {100.0f + 3820.0f, 50.0f + 2140.0f}));

        // Vue écrasée : rien.
        rects.clear();
        ScreenUi::Draw(scene, list, {0.0f, 0.0f}, {100.0f, 0.0f}, {.rects = &rects});
        CHECK(rects.empty());
        return true;
    }

    // Textures factices : « logo.png » (400 x 200) existe, le reste non.
    std::optional<UI::TextureInfo> FakeTextures(std::string_view path) {
        if (path == "textures/logo.png") {
            return UI::TextureInfo{7, {400, 200}};
        }
        return std::nullopt;
    }

    const UI::DrawCommand* FindCommand(const UI::DrawList& list, UI::TextureId texture) {
        for (const UI::DrawCommand& command : list.GetCommands()) {
            if (command.texture == texture) {
                return &command;
            }
        }
        return nullptr;
    }

    bool RunImageLayoutTests(const UI::Font& font) {
        UI::DrawList list(font);
        Scene scene;
        const Entity logo = scene.CreateEntity("Logo");
        auto& image = scene.Add<ScreenImageComponent>(logo, ScreenImageComponent{.image = "textures/logo.png",
                                                                                 .anchor = ScreenAnchor::TopLeft,
                                                                                 .offsetX = 20.0f, .offsetY = 10.0f});
        std::vector<ScreenUi::ItemRect> rects;
        const ScreenUi::DrawOptions options{.textures = FakeTextures, .rects = &rects};
        const auto draw = [&](Vector2f viewMin, Vector2f viewMax) {
            list.Clear();
            rects.clear();
            ScreenUi::Draw(scene, list, viewMin, viewMax, options);
        };

        // Taille du fichier, en pixels de référence ; un quad avec la texture de l'image.
        draw({0.0f, 0.0f}, {1920.0f, 1080.0f});
        CHECK(rects.size() == 1 && Near(rects[0].min, {20.0f, 10.0f}) && Near(rects[0].max, {420.0f, 210.0f}));
        const UI::DrawCommand* command = FindCommand(list, 7);
        CHECK(command && command->vertexCount == 6);
        // En 4K, tout double.
        draw({0.0f, 0.0f}, {3840.0f, 2160.0f});
        CHECK(Near(rects[0].min, {40.0f, 20.0f}) && Near(rects[0].max, {840.0f, 420.0f}));

        // Largeur seule : hauteur selon les proportions ; les deux : étirée.
        image.width = 100.0f;
        draw({0.0f, 0.0f}, {1920.0f, 1080.0f});
        CHECK(Near(rects[0].max - rects[0].min, {100.0f, 50.0f}));
        image.height = 100.0f;
        draw({0.0f, 0.0f}, {1920.0f, 1080.0f});
        CHECK(Near(rects[0].max - rects[0].min, {100.0f, 100.0f}));

        // Teinte et opacité : la couleur des sommets.
        image.tint = {1.0f, 0.5f, 0.25f};
        image.opacity = 0.5f;
        draw({0.0f, 0.0f}, {1920.0f, 1080.0f});
        const Math::LinearColor color = list.GetVertices()[FindCommand(list, 7)->firstVertex].color;
        CHECK(color == (Math::LinearColor{1.0f, 0.5f, 0.25f, 0.5f}));

        // Étirée sur la vue ; le décalage ne compte plus et ne se glisse pas.
        CHECK(ScreenUi::FindOffset(scene, logo).has_value());
        image.fit = ScreenImageFit::Stretch;
        draw({100.0f, 50.0f}, {900.0f, 650.0f});
        CHECK(Near(rects[0].min, {100.0f, 50.0f}) && Near(rects[0].max, {900.0f, 650.0f}));
        CHECK(!ScreenUi::FindOffset(scene, logo).has_value());
        CHECK(ScreenUi::IsScreenElement(scene, logo));

        // Couvre une vue 16:9 avec une image 2:1 : hauteur de la vue, débord rogné à gauche et à droite.
        image.fit = ScreenImageFit::Cover;
        draw({0.0f, 0.0f}, {1920.0f, 1080.0f});
        CHECK(Near(rects[0].min, {0.0f, 0.0f}) && Near(rects[0].max, {1920.0f, 1080.0f})); // rectangle découpé à la vue
        const UI::Vertex* first = &list.GetVertices()[FindCommand(list, 7)->firstVertex];
        CHECK(std::abs(first[0].uv.x - (2160.0f - 1920.0f) / 2.0f / 2160.0f) < 0.001f && first[0].uv.y == 0.0f);

        // Fichier absent ou vide : rien dans le jeu, un cadre gris dans l'éditeur (sélectionnable).
        image = ScreenImageComponent{.image = "textures/absent.png"};
        draw({0.0f, 0.0f}, {1920.0f, 1080.0f});
        CHECK(rects.empty() && list.GetVertices().empty());
        rects.clear();
        list.Clear();
        ScreenUi::Draw(scene, list, {0.0f, 0.0f}, {1920.0f, 1080.0f},
                       {.textures = FakeTextures, .rects = &rects, .placeholders = true});
        CHECK(rects.size() == 1 && Near(rects[0].max - rects[0].min, {256.0f, 144.0f}));
        CHECK(list.GetCommands().size() == 1 && list.GetCommands()[0].texture == UI::FontTexture);
        return true;
    }

    // Texte dans une police du projet : son atlas (une autre texture) et ses mesures ; police absente : celle de la
    // liste, le texte reste visible.
    bool RunFontTests(const UI::Font& font, const UI::Font& other) {
        UI::DrawList list(font);
        Scene scene;
        const Entity title = scene.CreateEntity("Titre");
        scene.Add<ScreenTextComponent>(title, ScreenTextComponent{.text = "Été", .font = "Polices/Autre.ttf", .size = 64.0f});
        constexpr UI::TextureId OtherTexture = 5;
        std::vector<std::string> asked;
        const ScreenUi::FontResolver fonts = [&](std::string_view path) -> std::optional<UI::FontRef> {
            asked.emplace_back(path);
            return path == "Polices/Autre.ttf" ? std::optional<UI::FontRef>(UI::FontRef{&other, OtherTexture}) : std::nullopt;
        };
        std::vector<ScreenUi::ItemRect> rects;
        ScreenUi::Draw(scene, list, {0.0f, 0.0f}, {1920.0f, 1080.0f}, {.rects = &rects, .fonts = fonts});
        CHECK(asked.size() == 1 && asked[0] == "Polices/Autre.ttf");
        CHECK(list.GetCommands().size() == 1 && list.GetCommands()[0].texture == OtherTexture);
        CHECK(rects.size() == 1 && Near(rects[0].max - rects[0].min, UI::DrawList::MeasureText(other, "Été", 64.0f)));

        // Fichier introuvable : la police de la liste.
        scene.Get<ScreenTextComponent>(title).font = "Polices/Absente.ttf";
        list.Clear();
        ScreenUi::Draw(scene, list, {0.0f, 0.0f}, {1920.0f, 1080.0f}, {.fonts = fonts});
        CHECK(!list.GetCommands().empty() && list.GetCommands()[0].texture == UI::FontTexture);

        // Sans police (Arimo) : le résolveur n'est pas appelé.
        scene.Get<ScreenTextComponent>(title).font.clear();
        asked.clear();
        list.Clear();
        ScreenUi::Draw(scene, list, {0.0f, 0.0f}, {1920.0f, 1080.0f}, {.fonts = fonts});
        CHECK(asked.empty() && list.GetCommands()[0].texture == UI::FontTexture);

        // Les rectangles qui suivent reviennent à l'atlas de l'interface : une commande par changement de texture.
        list.Clear();
        list.AddText({&other, OtherTexture}, {0.0f, 0.0f}, "A", 20.0f, {1.0f, 1.0f, 1.0f});
        list.AddRect({0.0f, 0.0f}, {4.0f, 4.0f}, {1.0f, 1.0f, 1.0f});
        CHECK(list.GetCommands().size() == 2 && list.GetCommands()[1].texture == UI::FontTexture);
        return true;
    }

    bool RunSaveTests() {
        ComponentRegistry components;
        RegisterEngineComponents(components);
        Scene scene;
        const Entity entity = scene.CreateEntity("Titre");
        scene.Add<ScreenTextComponent>(entity, ScreenTextComponent{.text = "Été", .size = 32.0f,
                                                                   .color = {1.0f, 0.5f, 0.0f},
                                                                   .anchor = ScreenAnchor::BottomRight,
                                                                   .offsetX = -12.0f, .offsetY = 7.0f});
        const std::string text = SceneSerializer::SaveToString(scene, components);
        CHECK(text.find(R"("anchor": "bottomRight")") != std::string::npos);

        Scene loaded;
        CHECK(SceneSerializer::LoadFromString(loaded, components, text).has_value());
        const Entity copy = loaded.FindByName("Titre");
        CHECK(copy != NullEntity);
        const auto* component = loaded.TryGet<ScreenTextComponent>(copy);
        CHECK(component && component->text == "Été" && component->size == 32.0f);
        CHECK(component->anchor == ScreenAnchor::BottomRight && component->offsetX == -12.0f && component->offsetY == 7.0f);
        CHECK(component->color == (Math::LinearColor{1.0f, 0.5f, 0.0f}));
        CHECK(component->font.empty());
        CHECK(components.Find("screenText")->FindField("font")->asset == AssetType::Font);
        scene.Get<ScreenTextComponent>(entity).font = "Polices/Rajdhani-Bold.ttf";
        Scene withFont;
        CHECK(SceneSerializer::LoadFromString(withFont, components, SceneSerializer::SaveToString(scene, components)).has_value());
        CHECK(withFont.Get<ScreenTextComponent>(withFont.FindByName("Titre")).font == "Polices/Rajdhani-Bold.ttf");

        // Image : chemin, remplissage et taille relus tels quels.
        const Entity logo = scene.CreateEntity("Logo");
        scene.Add<ScreenImageComponent>(logo, ScreenImageComponent{.image = "textures/écran titre.png", .opacity = 0.75f,
                                                                   .fit = ScreenImageFit::Cover, .width = 300.0f,
                                                                   .anchor = ScreenAnchor::Top, .offsetY = 40.0f});
        const std::string withImage = SceneSerializer::SaveToString(scene, components);
        CHECK(withImage.find(R"("fit": "cover")") != std::string::npos);
        Scene reloaded;
        CHECK(SceneSerializer::LoadFromString(reloaded, components, withImage).has_value());
        const auto* image = reloaded.TryGet<ScreenImageComponent>(reloaded.FindByName("Logo"));
        CHECK(image && image->image == "textures/écran titre.png" && image->opacity == 0.75f);
        CHECK(image->fit == ScreenImageFit::Cover && image->width == 300.0f && image->height == 0.0f);
        CHECK(image->anchor == ScreenAnchor::Top && image->offsetY == 40.0f);
        CHECK(components.Find("screenImage")->FindField("image")->asset == AssetType::Image);
        return true;
    }
}

bool RunScreenUiTests() {
    auto font = UI::Font::Load(Platform::GetExecutableDirectory() / "assets" / "fonts" / "Arimo.ttf");
    if (!font) {
        std::printf("ÉCHEC : police introuvable (%s)\n", font.error().c_str());
        return false;
    }
    // Une seconde police, lue dans un dossier au nom accentué (FreeType reçoit le fichier en mémoire, pas son chemin).
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / Platform::PathFromUtf8("flux-polices-été");
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    std::filesystem::copy_file(Platform::GetExecutableDirectory() / "assets" / "fonts" / "Arimo.ttf", folder / "Autre.ttf",
                               std::filesystem::copy_options::overwrite_existing, error);
    auto other = UI::Font::Load(folder / "Autre.ttf");
    std::filesystem::remove_all(folder, error);
    if (!other) {
        std::printf("ÉCHEC : police au chemin accentué (%s)\n", other.error().c_str());
        return false;
    }
    return RunLayoutTests(**font) && RunImageLayoutTests(**font) && RunFontTests(**font, **other) && RunSaveTests();
}
