// Interfaces .fluxui : placement par les ancres, retour d'un rectangle à ses réglages (éditeur), fichier, souris en jeu.
#include "Check.hpp"

#include "Flux/Graphics/Font.hpp"
#include "Flux/Interface/Interface.hpp"
#include "Flux/Platform/Filesystem.hpp"
#include "Flux/UI/DrawList.hpp"

#include <algorithm>
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
    // Fenêtres en jeu : actions des boutons, pile Échap, pause, croix, fenêtre mise devant.
    bool RunWindowTests() {
        Interface::Document document;
        const auto window = [&](std::string name, Vector2f position, bool open) {
            Interface::Element element = Interface::MakeElement(document, Interface::ElementType::Window);
            element.name = std::move(name);
            element.anchor = Interface::Anchor::TopLeft;
            element.position = position;
            element.size = {400.0f, 300.0f};
            element.visible = open;
            return element;
        };
        const auto button = [&](Interface::ButtonAction action, std::string target, Vector2f position) {
            Interface::Element element = Interface::MakeElement(document, Interface::ElementType::Button);
            element.anchor = Interface::Anchor::TopLeft;
            element.position = position;
            element.size = {100.0f, 40.0f};
            element.action = action;
            element.target = std::move(target);
            return element;
        };
        // Menu (ouvert) : un bouton qui ouvre Options, un qui quitte, un qui appelle une fonction.
        Interface::Element menu = window("Menu", {0.0f, 0.0f}, true);
        menu.closeOnEscape = false;
        menu.children = {button(Interface::ButtonAction::OpenWindow, "Options", {10.0f, 50.0f}),
                         button(Interface::ButtonAction::Quit, "", {10.0f, 100.0f}),
                         button(Interface::ButtonAction::CallFunction, "Sauver", {10.0f, 150.0f})};
        // Options (fermée, avec une croix) : un bouton « Retour » qui ferme sa fenêtre.
        Interface::Element options = window("Options", {200.0f, 100.0f}, false);
        options.closeButton = true;
        options.children = {button(Interface::ButtonAction::CloseParent, "", {10.0f, 200.0f})};
        // Pause (fermée) : ouverte par Échap, met le jeu en pause.
        Interface::Element pause = window("Pause", {1000.0f, 500.0f}, false);
        pause.openOnEscape = true;
        pause.pausesGame = true;
        const std::uint32_t menuId = menu.id;
        const std::uint32_t optionsId = options.id;
        const std::uint32_t pauseId = pause.id;
        document.roots = {menu, options, pause};

        Interface::Interaction interaction;
        const Vector2f min{0.0f, 0.0f};
        const Vector2f max{1920.0f, 1080.0f};
        const auto frame = [&](Vector2f mouse, bool down, bool pressed, bool released, bool escape = false) {
            Interface::Update(document, interaction, min, max, {mouse, down, pressed, released, true, escape});
        };
        const auto click = [&](Vector2f mouse) {
            frame(mouse, true, true, false);
            frame(mouse, false, false, true);
        };
        const auto escape = [&] { frame({1900.0f, 1000.0f}, false, false, false, true); };
        const auto isOpen = [&](std::uint32_t id) { return Interface::Find(document, id)->visible; };

        // Ouvrir par son nom : Options s'ouvre et passe devant.
        click({50.0f, 70.0f});
        CHECK(isOpen(optionsId) && document.roots.back().id == optionsId);
        // Un clic dans le Menu (là où Options ne le couvre pas) le remet devant.
        click({50.0f, 270.0f});
        CHECK(document.roots.back().id == menuId);
        // Quitter et fonction : laissés au moteur, une frame seulement.
        click({50.0f, 120.0f});
        CHECK(interaction.action == Interface::ButtonAction::Quit);
        click({50.0f, 170.0f});
        CHECK(interaction.action == Interface::ButtonAction::CallFunction && interaction.target == "Sauver");
        frame({50.0f, 170.0f}, false, false, false);
        CHECK(interaction.action == Interface::ButtonAction::None);

        // « Fermer sa fenêtre » : le bouton Retour ferme Options.
        click({220.0f, 320.0f});
        CHECK(!isOpen(optionsId));
        // La croix (au bout de la barre de titre de 40) ferme ; appuyée puis relâchée ailleurs, non.
        CHECK(Interface::SetWindowOpen(document, "Options", true) && isOpen(optionsId));
        frame({580.0f, 120.0f}, true, true, false);
        frame({500.0f, 200.0f}, false, false, true);
        CHECK(isOpen(optionsId) && Near(Interface::Find(document, optionsId)->position, {200.0f, 100.0f}));
        frame({580.0f, 120.0f}, false, false, false);
        CHECK(interaction.closeHovered == optionsId);
        click({580.0f, 120.0f});
        CHECK(!isOpen(optionsId));

        // Échap : rien d'ouvert qui se ferme ainsi -> ouvre Pause (le jeu se fige) ; encore -> la ferme.
        CHECK(!interaction.pausesGame);
        escape();
        CHECK(isOpen(pauseId) && interaction.escapeUsed && interaction.pausesGame);
        // Options ouverte par-dessus : Échap la ferme d'abord (la plus en avant), puis Pause.
        Interface::SetWindowOpen(document, "Options", true);
        escape();
        CHECK(!isOpen(optionsId) && isOpen(pauseId));
        escape();
        CHECK(!isOpen(pauseId) && !interaction.pausesGame);
        CHECK(isOpen(menuId)); // « Échap la ferme » décoché
        CHECK(!Interface::SetWindowOpen(document, "Inconnue", true));

        // Éditeur : une fenêtre fermée est placée quand on la montre (elle ou un de ses enfants sélectionné).
        CHECK(std::ranges::none_of(Interface::Arrange(document, min, max),
                                   [&](const Interface::Placed& item) { return item.element->id == optionsId; }));
        const std::uint32_t backId = Interface::Find(document, optionsId)->children[0].id;
        const std::vector<Interface::Placed> revealed = Interface::Arrange(document, min, max, std::nullopt, backId);
        CHECK(revealed.size() == Interface::Arrange(document, min, max).size() + 2);

        // Les nouveaux réglages passent par le fichier.
        auto loaded = Interface::LoadFromString(Interface::SaveToString(document));
        CHECK(loaded.has_value());
        const Interface::Element* back = Interface::Find(*loaded, pauseId);
        CHECK(back && back->openOnEscape && back->pausesGame && !back->visible);
        CHECK(Interface::Find(*loaded, optionsId)->closeButton);
        const Interface::Element* call = Interface::FindByName(*loaded, "Menu");
        CHECK(call && call->children[2].action == Interface::ButtonAction::CallFunction && call->children[2].target == "Sauver");
        return true;
    }

    // Onglets : une page affichée sous la barre, changée par un clic sur un onglet ou par un bouton.
    bool RunTabsTests() {
        Interface::Document document;
        Interface::Element tabs = Interface::MakeElement(document, Interface::ElementType::Tabs);
        tabs.anchor = Interface::Anchor::TopLeft;
        tabs.size = {600.0f, 400.0f};
        tabs.titleHeight = 40.0f;
        const auto page = [&](std::string name, bool shown) {
            Interface::Element element = Interface::MakeElement(document, Interface::ElementType::Frame);
            element.name = std::move(name);
            element.anchor = Interface::Anchor::Stretch;
            element.position = {};
            element.size = {};
            element.visible = shown;
            return element;
        };
        Interface::Element general = page("Général", true);
        Interface::Element sound = page("Son", false);
        Interface::Element next = Interface::MakeElement(document, Interface::ElementType::Button);
        next.anchor = Interface::Anchor::TopLeft;
        next.position = {20.0f, 20.0f};
        next.size = {200.0f, 50.0f};
        next.action = Interface::ButtonAction::ShowTab;
        next.target = "Son";
        general.children.push_back(next);
        const std::uint32_t generalId = general.id;
        const std::uint32_t soundId = sound.id;
        tabs.children = {general, sound};
        document.roots.push_back(tabs);
        const Vector2f min{0.0f, 0.0f};
        const Vector2f max{1920.0f, 1080.0f};
        const auto shown = [&] { return Interface::GetShownTab(*Interface::Find(document, tabs.id)); };
        const auto placedIds = [&](std::uint32_t reveal = 0) {
            std::vector<std::uint32_t> ids;
            for (const Interface::Placed& item : Interface::Arrange(document, min, max, std::nullopt, reveal)) {
                ids.push_back(item.element->id);
            }
            return ids;
        };

        // Seule la page affichée est placée, sous la barre (y 40 à 400), avec ses enfants.
        const std::vector<Interface::Placed> placed = Interface::Arrange(document, min, max);
        CHECK(placed.size() == 3 && placed[1].element->id == generalId && placed[2].element->id == next.id);
        CHECK(Near(placed[1].rect.min, {0.0f, 40.0f}) && Near(placed[1].rect.max, {600.0f, 400.0f}));
        // Onglets de largeurs égales (4 entre les deux) ; sous la barre, aucun onglet.
        CHECK(Interface::FindTabAt(*placed[0].element, placed[0].rect, 1.0f, {100.0f, 20.0f}) == 0);
        CHECK(Interface::FindTabAt(*placed[0].element, placed[0].rect, 1.0f, {500.0f, 20.0f}) == 1);
        CHECK(Interface::FindTabAt(*placed[0].element, placed[0].rect, 1.0f, {300.0f, 20.0f}) == -1);
        CHECK(Interface::FindTabAt(*placed[0].element, placed[0].rect, 1.0f, {500.0f, 60.0f}) == -1);
        // L'éditeur montre la page cachée qu'on a sélectionnée, sans changer le document.
        const std::vector<std::uint32_t> revealed = placedIds(soundId);
        CHECK(std::ranges::find(revealed, soundId) != revealed.end() && std::ranges::find(revealed, generalId) == revealed.end());
        CHECK(shown() == 0);

        Interface::Interaction interaction;
        const auto click = [&](Vector2f mouse) {
            Interface::Update(document, interaction, min, max, {mouse, true, true, false, true});
            Interface::Update(document, interaction, min, max, {mouse, false, false, true, true});
        };
        // Survol puis clic sur le deuxième onglet ; relâché ailleurs : rien ne change.
        Interface::Update(document, interaction, min, max, {{500.0f, 20.0f}, false, false, false, true});
        CHECK(interaction.hovered == tabs.id && interaction.hoveredTab == 1 && interaction.wantsMouse);
        Interface::Update(document, interaction, min, max, {{500.0f, 20.0f}, true, true, false, true});
        Interface::Update(document, interaction, min, max, {{100.0f, 20.0f}, false, false, true, true});
        CHECK(shown() == 0);
        click({500.0f, 20.0f});
        CHECK(shown() == 1 && !Interface::Find(document, generalId)->visible);
        click({100.0f, 20.0f});
        CHECK(shown() == 0);
        // Le bouton « Afficher un onglet » de la première page montre la seconde.
        click({100.0f, 80.0f});
        CHECK(interaction.clicked == next.id && shown() == 1);
        CHECK(Interface::ShowTab(document, "Général") && shown() == 0);
        CHECK(!Interface::ShowTab(document, "Inconnue") && !Interface::ShowTab(document, Interface::Find(document, tabs.id)->name));

        // Fichier : type, action et page affichée gardés.
        Interface::ShowTab(document, "Son");
        auto loaded = Interface::LoadFromString(Interface::SaveToString(document));
        CHECK(loaded.has_value());
        const Interface::Element* copy = Interface::Find(*loaded, tabs.id);
        CHECK(copy && copy->type == Interface::ElementType::Tabs && copy->titleHeight == 40.0f && Interface::GetShownTab(*copy) == 1);
        CHECK(Interface::Find(*loaded, next.id)->action == Interface::ButtonAction::ShowTab);
        return true;
    }
}

bool RunInterfaceTests() {
    auto font = UI::Font::Load(Platform::GetExecutableDirectory() / "assets" / "fonts" / "Arimo.ttf");
    if (!font) {
        std::printf("ÉCHEC : police introuvable (%s)\n", font.error().c_str());
        return false;
    }
    return RunLayoutTests() && RunDocumentTests() && RunInteractionTests(**font) && RunWindowTests() && RunTabsTests();
}
