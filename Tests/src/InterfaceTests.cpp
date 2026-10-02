// Interfaces .fluxui : placement par les ancres, retour d'un rectangle à ses réglages (éditeur), fichier, souris en jeu.
#include "Check.hpp"

#include "Flux/Graphics/Font.hpp"
#include "Flux/Interface/Interface.hpp"
#include "Flux/Platform/Filesystem.hpp"
#include "Flux/UI/DrawList.hpp"

#include <cmath>

using namespace Flux;
using Math::Vector2f;

namespace {
    bool Near(Vector2f a, Vector2f b) {
        return std::abs(a.x - b.x) < 0.01f && std::abs(a.y - b.y) < 0.01f;
    }

    bool RunLayoutTests() {
        const Interface::Rect screen{{0.0f, 0.0f}, {1920.0f, 1080.0f}};
        Interface::Element element = Interface::MakeDefault(Interface::ElementType::Button);
        element.size = {200.0f, 50.0f};

        // Au centre : centre sur centre, puis le décalage.
        element.anchor = Interface::Anchor::Center;
        element.position = {10.0f, -20.0f};
        Interface::Rect rect = Interface::Arrange(element, screen, 1.0f);
        CHECK(Near(rect.min, {860.0f + 10.0f, 515.0f - 20.0f}));
        CHECK(Near(rect.max - rect.min, {200.0f, 50.0f}));

        // En bas à droite, échelle 2 (écran 4K) : coin sur coin, tout double.
        element.anchor = Interface::Anchor::BottomRight;
        element.position = {-10.0f, -10.0f};
        rect = Interface::Arrange(element, {{0.0f, 0.0f}, {3840.0f, 2160.0f}}, 2.0f);
        CHECK(Near(rect.max, {3820.0f, 2140.0f}));
        CHECK(Near(rect.max - rect.min, {400.0f, 100.0f}));

        // Étiré : marges gauche-haut (position) et droite-bas (taille).
        element.anchor = Interface::Anchor::Stretch;
        element.position = {20.0f, 30.0f};
        element.size = {40.0f, 50.0f};
        rect = Interface::Arrange(element, screen, 1.0f);
        CHECK(Near(rect.min, {20.0f, 30.0f}) && Near(rect.max, {1880.0f, 1030.0f}));

        // SetRect est l'inverse d'Arrange, quelle que soit l'ancre : l'éditeur déplace sans connaître l'ancre.
        const Interface::Rect wanted{{100.0f, 200.0f}, {400.0f, 260.0f}};
        for (int anchor = 0; anchor < static_cast<int>(Interface::Anchor::Count); ++anchor) {
            element.anchor = static_cast<Interface::Anchor>(anchor);
            Interface::SetRect(element, screen, 1.5f, wanted);
            const Interface::Rect back = Interface::Arrange(element, screen, 1.5f);
            CHECK(Near(back.min, wanted.min) && Near(back.max, wanted.max));
        }

        // Document : enfants placés dans leur parent et découpés par lui ; éléments masqués absents.
        Interface::Document document;
        Interface::Element window = Interface::MakeElement(document, Interface::ElementType::Window);
        window.anchor = Interface::Anchor::TopLeft;
        window.size = {600.0f, 400.0f};
        Interface::Element child = Interface::MakeElement(document, Interface::ElementType::Text);
        child.anchor = Interface::Anchor::TopRight;
        child.position = {100.0f, 0.0f}; // déborde à droite de la fenêtre
        child.size = {200.0f, 40.0f};
        Interface::Element hidden = Interface::MakeElement(document, Interface::ElementType::Image);
        hidden.visible = false;
        window.children = {child, hidden};
        document.roots.push_back(window);
        const std::vector<Interface::Placed> placed = Interface::Arrange(document, screen.min, screen.max);
        CHECK(placed.size() == 2);
        CHECK(placed[1].parentId == window.id && placed[1].depth == 1);
        CHECK(Near(placed[1].rect.min, {500.0f, 0.0f}));
        CHECK(Near(placed[1].clip.max, {600.0f, 400.0f}));
        // Sous la souris : l'enfant devant la fenêtre, mais seulement dans sa partie visible.
        CHECK(Interface::FindAt(placed, {550.0f, 20.0f}) == child.id);
        CHECK(Interface::FindAt(placed, {650.0f, 20.0f}) == 0);
        CHECK(Interface::FindAt(placed, {100.0f, 300.0f}) == window.id);
        return true;
    }

    bool RunDocumentTests() {
        Interface::Document document;
        Interface::Element window = Interface::MakeElement(document, Interface::ElementType::Window);
        window.name = "Réglages";
        window.text = "Réglages du jeu";
        window.titleColor = {0.1f, 0.2f, 0.3f, 0.75f};
        Interface::Element slider = Interface::MakeElement(document, Interface::ElementType::Slider);
        slider.value = 0.25f;
        slider.anchor = Interface::Anchor::Stretch;
        Interface::Element box = Interface::MakeElement(document, Interface::ElementType::Checkbox);
        box.checked = true;
        window.children = {slider, box};
        document.roots.push_back(window);
        document.roots.push_back(Interface::MakeElement(document, Interface::ElementType::Image));

        // Aller-retour par le texte : identique, et réécrit à l'identique.
        const std::string text = Interface::SaveToString(document);
        auto loaded = Interface::LoadFromString(text);
        CHECK(loaded.has_value());
        CHECK(Interface::SaveToString(*loaded) == text);
        CHECK(loaded->nextId == document.nextId);
        const Interface::Element* back = Interface::Find(*loaded, window.id);
        CHECK(back && back->name == "Réglages" && back->text == "Réglages du jeu");
        CHECK(back->titleColor == window.titleColor && back->children.size() == 2);
        CHECK(back->children[0].value == 0.25f && back->children[0].anchor == Interface::Anchor::Stretch);
        CHECK(back->children[1].checked);

        // Fichiers refusés : autre format, type inconnu, identifiants en double.
        CHECK(!Interface::LoadFromString("{\"format\":\"flux.scene\",\"version\":1}"));
        CHECK(!Interface::LoadFromString(
            R"({"format":"flux.ui","version":1,"elements":[{"id":1,"type":"robot"}]})"));
        CHECK(!Interface::LoadFromString(
            R"({"format":"flux.ui","version":1,"elements":[{"id":1,"type":"text"},{"id":1,"type":"text"}]})"));
        // nextId absent ou trop petit : jamais un identifiant déjà pris.
        auto small = Interface::LoadFromString(R"({"format":"flux.ui","version":1,"nextId":1,"elements":[{"id":7,"type":"text"}]})");
        CHECK(small && small->nextId == 8);

        // Retirer, chercher le parent, dupliquer avec de nouveaux identifiants.
        Interface::Document edited = *loaded;
        CHECK(Interface::FindParent(edited, box.id)->id == window.id);
        CHECK(Interface::FindParent(edited, window.id) == nullptr);
        const Interface::Element copy = Interface::CloneWithNewIds(edited, *Interface::Find(edited, window.id));
        CHECK(copy.id != window.id && copy.children[0].id != slider.id && copy.children[1].id >= document.nextId);
        CHECK(Interface::Remove(edited, slider.id).has_value());
        CHECK(Interface::Find(edited, slider.id) == nullptr && Interface::Find(edited, window.id)->children.size() == 1);
        return true;
    }

    bool RunInteractionTests(const UI::Font& font) {
        Interface::Document document;
        Interface::Element window = Interface::MakeElement(document, Interface::ElementType::Window);
        window.anchor = Interface::Anchor::TopLeft;
        window.size = {600.0f, 400.0f};
        window.titleHeight = 40.0f;
        Interface::Element button = Interface::MakeElement(document, Interface::ElementType::Button);
        button.anchor = Interface::Anchor::TopLeft;
        button.position = {20.0f, 60.0f};
        button.size = {200.0f, 50.0f};
        Interface::Element box = Interface::MakeElement(document, Interface::ElementType::Checkbox);
        box.anchor = Interface::Anchor::TopLeft;
        box.position = {20.0f, 140.0f};
        box.size = {200.0f, 40.0f};
        Interface::Element slider = Interface::MakeElement(document, Interface::ElementType::Slider);
        slider.anchor = Interface::Anchor::TopLeft;
        slider.position = {20.0f, 220.0f};
        slider.size = {220.0f, 20.0f}; // poignée de 10 de rayon : le centre glisse de x 30 à 230
        slider.value = 0.0f;
        window.children = {button, box, slider};
        document.roots.push_back(window);
        const Vector2f min{0.0f, 0.0f};
        const Vector2f max{1920.0f, 1080.0f};

        Interface::Interaction interaction;
        const auto frame = [&](Vector2f mouse, bool down, bool pressed, bool released) {
            Interface::Update(document, interaction, min, max, {mouse, down, pressed, released, true});
        };
        const auto click = [&](Vector2f mouse) {
            frame(mouse, true, true, false);
            frame(mouse, false, false, true);
        };

        // Survol : bouton survolé, et l'interface retient la souris (le jeu ignore ses clics) ; à côté, rien.
        frame({100.0f, 80.0f}, false, false, false);
        CHECK(interaction.hovered == button.id && interaction.wantsMouse);
        frame({1000.0f, 800.0f}, false, false, false);
        CHECK(interaction.hovered == 0 && !interaction.wantsMouse);

        // Clic sur le bouton : cliqué au relâchement seulement, et seulement s'il est relâché dessus.
        frame({100.0f, 80.0f}, true, true, false);
        CHECK(interaction.clicked == 0 && interaction.pressed == button.id);
        frame({100.0f, 80.0f}, false, false, true);
        CHECK(interaction.clicked == button.id);
        frame({100.0f, 80.0f}, true, true, false);
        frame({1000.0f, 80.0f}, false, false, true);
        CHECK(interaction.clicked == 0);

        // Case : cochée puis décochée.
        click({40.0f, 160.0f});
        CHECK(Interface::Find(document, box.id)->checked);
        click({40.0f, 160.0f});
        CHECK(!Interface::Find(document, box.id)->checked);

        // Curseur : la valeur suit la souris tant que le bouton est tenu, bornée.
        frame({130.0f, 230.0f}, true, true, false);
        CHECK(std::abs(Interface::Find(document, slider.id)->value - 0.5f) < 0.001f);
        frame({900.0f, 230.0f}, true, false, false);
        CHECK(Interface::Find(document, slider.id)->value == 1.0f);
        frame({900.0f, 230.0f}, false, false, true);

        // Fenêtre déplacée par sa barre de titre (pas par son fond).
        frame({300.0f, 20.0f}, true, true, false);
        frame({350.0f, 70.0f}, true, false, false);
        frame({350.0f, 70.0f}, false, false, true);
        CHECK(Near(Interface::Find(document, window.id)->position, {50.0f, 50.0f}));
        frame({300.0f, 350.0f}, true, true, false);
        frame({400.0f, 450.0f}, true, false, false);
        frame({400.0f, 450.0f}, false, false, true);
        CHECK(Near(Interface::Find(document, window.id)->position, {50.0f, 50.0f}));

        // Souris prise ailleurs (panneau de l'éditeur) : rien n'est survolé.
        Interface::Update(document, interaction, min, max, {{100.0f, 130.0f}, false, false, false, false});
        CHECK(interaction.hovered == 0 && !interaction.wantsMouse);

        // Dessin : quelque chose, et découpé à la vue.
        UI::DrawList list(font);
        Interface::Draw(document, list, min, max, {.interaction = &interaction, .placeholders = true});
        CHECK(!list.GetVertices().empty());
        return true;
    }
}

bool RunInterfaceTests() {
    auto font = UI::Font::Load(Platform::GetExecutableDirectory() / "assets" / "fonts" / "Arimo.ttf");
    if (!font) {
        std::printf("ÉCHEC : police introuvable (%s)\n", font.error().c_str());
        return false;
    }
    return RunLayoutTests() && RunDocumentTests() && RunInteractionTests(**font);
}
