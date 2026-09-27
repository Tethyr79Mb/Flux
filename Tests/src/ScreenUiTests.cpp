// Textes à l'écran : placement selon l'ancre, le décalage et la taille de la vue ; sauvegarde du composant.
#include "Check.hpp"

#include "Flux/Platform/Filesystem.hpp"
#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/SceneSerializer.hpp"
#include "Flux/Scene/ScreenUi.hpp"
#include "Flux/UI/DrawList.hpp"
#include "Flux/UI/Font.hpp"

#include <cmath>
#include <string>
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
        ScreenUi::Draw(scene, list, {0.0f, 0.0f}, {1920.0f, 1080.0f}, &rects);
        CHECK(rects.size() == 2);
        CHECK(rects[0].entity == title && rects[1].entity == corner); // ordre de la hiérarchie
        const Vector2f titleSize = list.MeasureText("Ars Solaris", 64.0f);
        CHECK(Near(rects[0].min, Vector2f{960.0f, 540.0f} - titleSize * 0.5f));
        CHECK(Near(rects[0].max - rects[0].min, titleSize));
        // En bas à droite : le coin bas-droit du texte sur le coin de l'écran, puis le décalage.
        CHECK(Near(rects[1].max, {1910.0f, 1070.0f}));

        // Vue deux fois plus haute (4K) : tout double, et la vue décalée (éditeur) décale tout.
        rects.clear();
        ScreenUi::Draw(scene, list, {100.0f, 50.0f}, {100.0f + 3840.0f, 50.0f + 2160.0f}, &rects);
        CHECK(Near(rects[0].max - rects[0].min, list.MeasureText("Ars Solaris", 128.0f)));
        CHECK(Near(rects[1].max, {100.0f + 3820.0f, 50.0f + 2140.0f}));

        // Vue écrasée : rien.
        rects.clear();
        ScreenUi::Draw(scene, list, {0.0f, 0.0f}, {100.0f, 0.0f}, &rects);
        CHECK(rects.empty());
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
        return true;
    }
}

bool RunScreenUiTests() {
    auto font = UI::Font::Load(Platform::GetExecutableDirectory() / "assets" / "fonts" / "Arimo.ttf");
    if (!font) {
        std::printf("ÉCHEC : police introuvable (%s)\n", font.error().c_str());
        return false;
    }
    return RunLayoutTests(**font) && RunSaveTests();
}
