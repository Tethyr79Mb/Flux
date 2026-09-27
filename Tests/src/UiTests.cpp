// Tests de FluxUI sans fenêtre ni GPU : souris et clavier simulés frame par frame.
#include "Check.hpp"

#include "Flux/Math/Quaternion.hpp"
#include "Flux/Platform/Filesystem.hpp"
#include "Flux/Platform/Input.hpp"
#include "Flux/UI/Context.hpp"
#include "Flux/UI/DrawList.hpp"
#include "Flux/UI/Font.hpp"

#include <cmath>
#include <functional>
#include <string>

using namespace Flux;
using Math::Vector2f;

namespace {
    // Pilote une frame : événements, puis description de l'interface.
    class Harness {
        public:
        explicit Harness(const UI::Font& font) : m_ui(font), m_output(font) {}

        void Frame(const std::function<void()>& events, const std::function<void(UI::Context&)>& describe) {
            m_input.BeginFrame();
            events();
            m_output.Clear();
            m_ui.BeginFrame(m_input, {1280.0f, 720.0f}, 1.0 / 60.0);
            describe(m_ui);
            m_ui.EndFrame(m_output);
        }

        void Move(Vector2f position, const std::function<void(UI::Context&)>& describe) {
            Frame([&] { m_input.SetMousePosition(position); }, describe);
        }

        void Click(Vector2f position, const std::function<void(UI::Context&)>& describe) {
            Move(position, describe);
            Frame([&] { m_input.SetMouseButton(MouseButton::Left, true); }, describe);
            Frame([&] { m_input.SetMouseButton(MouseButton::Left, false); }, describe);
        }

        void Type(std::string_view text, const std::function<void(UI::Context&)>& describe) {
            Frame([&] {
                for (const char c : text) {
                    m_input.AddCharacter(static_cast<unsigned char>(c)); // ASCII seulement dans ces tests
                }
            }, describe);
        }

        void Key(Flux::Key key, const std::function<void(UI::Context&)>& describe) {
            Frame([&] { m_input.SetKey(static_cast<std::uint16_t>(key), true); }, describe);
            Frame([&] { m_input.SetKey(static_cast<std::uint16_t>(key), false); }, describe);
        }

        Input& GetInput() { return m_input; }
        UI::Context& GetUi() { return m_ui; }

        private:
        Input m_input;
        UI::Context m_ui;
        UI::DrawList m_output;
    };

    bool RunWidgetTests(const UI::Font& font) {
        Harness harness(font);
        const float row = font.GetLineHeight() * 15.0f + 8.0f; // hauteur d'un champ (style par défaut)
        const float top = row + 8.0f;                          // sous la barre de titre, après la marge
        const auto rowCenter = [&](int index) { return top + static_cast<float>(index) * (row + 4.0f) + row * 0.5f; };

        bool check = true;
        double number = 10.0;
        std::string name = "Soleil";
        int clicks = 0;
        const auto describe = [&](UI::Context& ui) {
            if (ui.BeginPanel("Test", {0.0f, 0.0f}, 400.0f)) {
                ui.Checkbox("Case", check);                // ligne 0
                ui.DragDouble("Nombre", number, 1.0, 0.0); // ligne 1
                ui.InputText("Nom", name);                 // ligne 2
                if (ui.Button("Bouton")) {                 // ligne 3
                    ++clicks;
                }
            }
            ui.EndPanel();
        };
        const float fieldX = 8.0f + std::floor(384.0f * 0.38f) + 30.0f;

        harness.Move({600.0f, 600.0f}, describe);
        harness.Move({600.0f, 600.0f}, describe);
        CHECK(!harness.GetUi().WantsMouse());
        harness.Move({20.0f, rowCenter(0)}, describe);
        CHECK(harness.GetUi().WantsMouse());

        harness.Click({20.0f, rowCenter(0)}, describe);
        CHECK(!check);

        // Glisser de 30 pixels : +30 ; un clic sans glisser ouvre la saisie.
        harness.Move({fieldX, rowCenter(1)}, describe);
        harness.Frame([&] { harness.GetInput().SetMouseButton(MouseButton::Left, true); }, describe);
        harness.Move({fieldX + 30.0f, rowCenter(1)}, describe);
        harness.Frame([&] { harness.GetInput().SetMouseButton(MouseButton::Left, false); }, describe);
        CHECK(std::abs(number - 40.0) < 1e-9);
        CHECK(!harness.GetUi().WantsKeyboard());

        harness.Click({fieldX, rowCenter(1)}, describe);
        CHECK(harness.GetUi().WantsKeyboard());
        harness.Type("2,5", describe); // virgule française, remplace tout le texte sélectionné
        harness.Key(Key::Enter, describe);
        CHECK(number == 2.5);
        CHECK(!harness.GetUi().WantsKeyboard());

        // Valeur invalide ou sous le minimum.
        harness.Click({fieldX, rowCenter(1)}, describe);
        harness.Type("--", describe);
        harness.Key(Key::Enter, describe);
        CHECK(number == 2.5);
        harness.Click({fieldX, rowCenter(1)}, describe);
        harness.Type("-3", describe);
        harness.Key(Key::Enter, describe);
        CHECK(number == 0.0);

        // Texte : Échap annule ; retour arrière efface un caractère UTF-8 entier (é = 2 octets).
        harness.Click({fieldX, rowCenter(2)}, describe);
        harness.Type("Lune", describe);
        harness.Key(Key::Escape, describe);
        CHECK(name == "Soleil");
        name = "Planète";
        harness.Click({fieldX, rowCenter(2)}, describe);
        harness.Key(Key::End, describe);
        harness.Key(Key::Backspace, describe);
        harness.Key(Key::Backspace, describe);
        harness.Click({1000.0f, 600.0f}, describe); // clic ailleurs : valide
        CHECK(name == "Planè");
        harness.Click({fieldX, rowCenter(2)}, describe);
        harness.Key(Key::Backspace, describe);
        harness.Type("Terre", describe);
        harness.Key(Key::Enter, describe);
        CHECK(name == "Terre");

        // Bouton : relâcher ailleurs n'est pas un clic.
        harness.Click({20.0f, rowCenter(3)}, describe);
        CHECK(clicks == 1);
        harness.Move({20.0f, rowCenter(3)}, describe);
        harness.Frame([&] { harness.GetInput().SetMouseButton(MouseButton::Left, true); }, describe);
        harness.Move({600.0f, 600.0f}, describe);
        harness.Frame([&] { harness.GetInput().SetMouseButton(MouseButton::Left, false); }, describe);
        CHECK(clicks == 1);

        // Replier le panneau : le contenu n'est plus survolable.
        harness.Click({200.0f, row * 0.5f}, describe);
        harness.Move({20.0f, rowCenter(0)}, describe);
        CHECK(!harness.GetUi().WantsMouse());
        harness.Click({200.0f, row * 0.5f}, describe);
        return true;
    }

    bool RunTreeTests(const UI::Font& font) {
        Harness harness(font);
        const float titleHeight = font.GetLineHeight() * 15.0f + 8.0f;
        const float treeRow = font.GetLineHeight() * 15.0f + 4.0f + 1.0f; // hauteur + espacement des nœuds
        const auto rowCenter = [&](int index) { return titleHeight + 8.0f + static_cast<float>(index) * treeRow + treeRow * 0.5f; };

        int selected = -1;
        bool childVisible = false;
        const auto describe = [&](UI::Context& ui) {
            childVisible = false;
            // Hauteur fixe de 5 lignes pour 40 nœuds : le panneau défile.
            if (ui.BeginPanel("Arbre", {0.0f, 0.0f}, 300.0f, titleHeight + 8.0f + 5.0f * treeRow)) {
                for (int i = 0; i < 40; ++i) {
                    ui.PushId(static_cast<std::uint64_t>(i));
                    const auto node = ui.TreeNode("Nœud", selected == i, i != 0);
                    if (node.clicked) {
                        selected = i;
                    }
                    if (node.open) {
                        childVisible = true;
                        if (ui.TreeNode("Enfant", false, true).clicked) {
                            selected = 100;
                        }
                        ui.TreePop();
                    }
                    ui.PopId();
                }
            }
            ui.EndPanel();
        };

        harness.Move({100.0f, rowCenter(0)}, describe);
        harness.Click({100.0f, rowCenter(0)}, describe);
        CHECK(selected == 0);
        CHECK(!childVisible);
        harness.Click({8.0f + 6.0f, rowCenter(0)}, describe); // flèche : déplie sans sélectionner
        CHECK(childVisible);
        CHECK(selected == 0);
        harness.Click({100.0f, rowCenter(1)}, describe);      // l'enfant, en ligne 1
        CHECK(selected == 100);
        harness.Click({8.0f + 6.0f, rowCenter(0)}, describe); // replier
        CHECK(!childVisible);

        // Double-clic sur le libellé : déplie aussi.
        harness.Click({100.0f, rowCenter(0)}, describe);
        harness.Click({100.0f, rowCenter(0)}, describe);
        CHECK(childVisible);
        harness.Click({8.0f + 6.0f, rowCenter(0)}, describe);

        // Molette : trois lignes par cran ; le nœud 3 passe en première ligne visible.
        harness.Frame([&] { harness.GetInput().AddWheelDelta(-1.0f); }, describe);
        harness.Click({100.0f, rowCenter(0)}, describe);
        CHECK(selected == 3);
        // Sous le bas du panneau : rien n'est cliquable, même si le nœud y serait dessiné sans découpe.
        selected = -1;
        harness.Click({100.0f, rowCenter(6)}, describe);
        CHECK(selected == -1);
        // Défilement borné : on ne descend pas plus bas que le dernier nœud (molette au-dessus du panneau).
        harness.Move({100.0f, rowCenter(2)}, describe);
        harness.Frame([&] { harness.GetInput().AddWheelDelta(-100.0f); }, describe);
        harness.Frame([] {}, describe);
        harness.Click({100.0f, rowCenter(4)}, describe);
        CHECK(selected == 39);
        return true;
    }

    // Liste déroulante (Choice) et bouton tenu (IsItemActive, début d'un glisser-déposer).
    bool RunChoiceTests(const UI::Font& font) {
        Harness harness(font);
        const float row = font.GetLineHeight() * 15.0f + 8.0f;
        const float top = row + 8.0f;
        const float fieldX = 8.0f + std::floor(384.0f * 0.38f) + 30.0f;
        const float fieldY = top + row * 0.5f;
        const auto optionY = [&](int index) { return top + row + 4.0f + static_cast<float>(index) * row + row * 0.5f; };

        constexpr std::string_view Options[] = {"Gauche", "Centre", "Droite"};
        int index = 0;
        int changes = 0;
        bool held = false;
        const auto describe = [&](UI::Context& ui) {
            if (ui.BeginPanel("Test", {0.0f, 0.0f}, 400.0f)) {
                if (ui.Choice("Ancre", index, Options)) { // ligne 0
                    ++changes;
                }
                ui.Button("Tenir"); // ligne 1
                held = ui.IsItemActive();
            }
            ui.EndPanel();
        };

        harness.Move({600.0f, 600.0f}, describe);
        harness.Move({600.0f, 600.0f}, describe);
        // Clic sur le champ : la liste s'ouvre sous lui, sans rien changer ; clic sur une valeur : choisie.
        harness.Click({fieldX, fieldY}, describe);
        CHECK(index == 0 && changes == 0);
        harness.Click({fieldX, optionY(2)}, describe);
        CHECK(index == 2 && changes == 1);
        // Liste refermée : un clic à la place de ses valeurs n'y touche plus.
        harness.Click({fieldX, optionY(1)}, describe);
        CHECK(index == 2);
        // Rouvrir, puis cliquer ailleurs : refermée sans changement.
        harness.Click({fieldX, fieldY}, describe);
        harness.Click({1000.0f, 600.0f}, describe);
        harness.Click({fieldX, optionY(0)}, describe);
        CHECK(index == 2 && changes == 1);

        // Bouton tenu : actif jusqu'au relâchement, même souris sortie du bouton.
        const float buttonY = top + row + 4.0f + row * 0.5f;
        harness.Move({20.0f, buttonY}, describe);
        harness.Frame([&] { harness.GetInput().SetMouseButton(MouseButton::Left, true); }, describe);
        CHECK(held);
        harness.Move({700.0f, 500.0f}, describe);
        CHECK(held);
        harness.Frame([&] { harness.GetInput().SetMouseButton(MouseButton::Left, false); }, describe);
        CHECK(!held);
        return true;
    }

    bool RunMenuTests(const UI::Font& font) {
        Harness harness(font);
        const float bar = font.GetLineHeight() * 15.0f + 8.0f; // hauteur de la barre = hauteur d'un champ
        const float fileX = 4.0f + 10.0f;                       // dans le titre « Fichier »
        const auto itemY = [&](int index) { return bar + 4.0f + static_cast<float>(index) * bar + bar * 0.5f; };

        int saves = 0;
        int opens = 0;
        int buttonClicks = 0;
        bool menuOpen = false;
        const auto describe = [&](UI::Context& ui) {
            // Panneau demandé en haut de l'écran : repoussé sous la barre.
            if (ui.BeginPanel("Dessous", {300.0f, 0.0f}, 400.0f)) {
                if (ui.Button("Bouton")) {
                    ++buttonClicks;
                }
            }
            ui.EndPanel();
            menuOpen = false;
            if (ui.BeginMainMenuBar()) {
                if (ui.BeginMenu("Fichier")) {
                    menuOpen = true;
                    if (ui.MenuItem("Ouvrir...", "Ctrl+O")) {
                        ++opens;
                    }
                    if (ui.MenuItem("Enregistrer", "Ctrl+S")) {
                        ++saves;
                    }
                    ui.MenuItem("Grisé", {}, false);
                    ui.EndMenu();
                }
                ui.EndMainMenuBar();
            }
        };

        harness.Move({600.0f, 600.0f}, describe);
        harness.Move({600.0f, 600.0f}, describe);
        CHECK(harness.GetUi().GetMenuBarHeight() == bar);
        CHECK(!menuOpen);

        // Clic sur le titre : le menu s'ouvre ; clic sur un élément : il agit et referme le menu.
        harness.Click({fileX, bar * 0.5f}, describe);
        CHECK(menuOpen);
        harness.Click({40.0f, itemY(1)}, describe);
        CHECK(saves == 1 && opens == 0);
        harness.Move({40.0f, itemY(1)}, describe);
        CHECK(!menuOpen);

        // Élément désactivé : rien, le menu reste ouvert.
        harness.Click({fileX, bar * 0.5f}, describe);
        harness.Click({40.0f, itemY(2)}, describe);
        harness.Move({40.0f, itemY(2)}, describe);
        CHECK(menuOpen && saves == 1);

        // Clic ailleurs : referme le menu, sans atteindre le bouton du panneau en dessous.
        // Le panneau commence sous la barre : titre, marge, puis le bouton.
        const float buttonY = bar + bar + 8.0f + bar * 0.5f;
        harness.Click({320.0f, buttonY}, describe);
        CHECK(!menuOpen);
        CHECK(buttonClicks == 0);
        harness.Click({320.0f, buttonY}, describe);
        CHECK(buttonClicks == 1);

        // Menu contextuel : clic droit sur le bouton, puis clic sur un élément.
        int deletes = 0;
        bool contextOpen = false;
        const auto describeContext = [&](UI::Context& ui) {
            contextOpen = false;
            if (ui.BeginPanel("Dessous", {300.0f, 0.0f}, 400.0f)) {
                ui.Button("Bouton");
                if (ui.BeginContextMenu()) {
                    contextOpen = true;
                    if (ui.MenuItem("Supprimer")) {
                        ++deletes;
                    }
                    ui.EndMenu();
                }
            }
            ui.EndPanel();
        };
        harness.Move({320.0f, buttonY}, describeContext);
        harness.Frame([&] { harness.GetInput().SetMouseButton(MouseButton::Right, true); }, describeContext);
        harness.Frame([&] { harness.GetInput().SetMouseButton(MouseButton::Right, false); }, describeContext);
        CHECK(contextOpen);
        // Le menu apparaît sous la souris : son premier élément est juste en dessous.
        harness.Click({330.0f, buttonY + 4.0f + bar * 0.5f}, describeContext);
        harness.Move({330.0f, buttonY + 4.0f + bar * 0.5f}, describeContext);
        CHECK(deletes == 1 && !contextOpen);

        // Échap referme aussi.
        harness.Click({fileX, bar * 0.5f}, describe);
        CHECK(menuOpen);
        harness.Key(Key::Escape, describe);
        CHECK(!menuOpen);
        return true;
    }

    bool RunDockTests(const UI::Font& font) {
        Harness harness(font);
        UI::Context& ui = harness.GetUi();
        const float bar = font.GetLineHeight() * 15.0f + 8.0f; // hauteur de la barre d'onglets

        // Un panneau à gauche (25 %), deux onglets à droite (30 % du reste), la vue au milieu.
        const UI::DockId root = ui.ClearDockLayout();
        const auto [left, rest] = ui.SplitDock(root, UI::DockSide::Left, 0.25f);
        const UI::DockId right = ui.SplitDock(rest, UI::DockSide::Right, 0.3f).side;
        ui.DockPanel("Gauche", left);
        ui.DockPanel("A", right);
        ui.DockPanel("B", right);

        UI::Rect view;
        bool leftOpen = false;
        bool aOpen = false;
        bool bOpen = false;
        bool floatingSeen = false;
        const auto describe = [&](UI::Context& context) {
            view = context.DockSpace({0.0f, 0.0f}, {1280.0f, 720.0f});
            leftOpen = context.BeginPanel("Gauche", {0.0f, 0.0f});
            if (leftOpen) {
                context.Text("contenu");
            }
            context.EndPanel();
            aOpen = context.BeginPanel("A", {0.0f, 0.0f});
            context.EndPanel();
            bOpen = context.BeginPanel("B", {0.0f, 0.0f});
            context.EndPanel();
            // Panneau flottant demandé à gauche de l'écran : ramené dans la zone centrale.
            floatingSeen = context.BeginPanel("Flottant", {10.0f, 10.0f}, 200.0f);
            context.EndPanel();
        };

        harness.Move({640.0f, 400.0f}, describe);
        harness.Move({640.0f, 400.0f}, describe);
        // 1280 - 4 (séparateur) = 1276 ; 25 % : 319, la vue commence après le séparateur.
        CHECK(view.min.x == 323.0f && view.min.y == 0.0f && view.max.y == 720.0f);
        CHECK(aOpen && !bOpen && floatingSeen);
        CHECK(!ui.WantsMouse()); // milieu de la vue : à la caméra

        // Le panneau flottant est dans la vue, et la souris y est prise par l'UI.
        harness.Move({view.min.x + 20.0f, 20.0f}, describe); // demandé en (10, 10) : x ramené dans la vue
        CHECK(ui.WantsMouse());

        // Zone ancrée, séparateur : l'UI prend la souris.
        harness.Move({100.0f, 400.0f}, describe);
        CHECK(ui.WantsMouse());
        harness.Move({321.0f, 400.0f}, describe);
        CHECK(ui.WantsMouse());

        // Onglets : clic sur le second.
        const float tabY = bar * 0.5f + 2.0f;
        const float rightX = view.max.x + 4.0f;
        const float tabAWidth = ui.MeasureText("A").x + 2.0f * 6.0f + 8.0f; // texte, marges, espace
        const float tabBX = rightX + 2.0f + tabAWidth + 2.0f + 10.0f;
        harness.Click({tabBX, tabY}, describe);
        harness.Move({tabBX, tabY}, describe);
        CHECK(!aOpen && bOpen);

        // Glisser le séparateur de gauche jusqu'à x = 500.
        harness.Move({321.0f, 400.0f}, describe);
        harness.Frame([&] { harness.GetInput().SetMouseButton(MouseButton::Left, true); }, describe);
        harness.Move({500.0f, 400.0f}, describe);
        harness.Frame([&] { harness.GetInput().SetMouseButton(MouseButton::Left, false); }, describe);
        harness.Move({640.0f, 400.0f}, describe);
        CHECK(std::abs(view.min.x - 502.0f) <= 1.0f);

        // Enregistrement puis rechargement : même disposition, même onglet affiché.
        const std::string saved = ui.SaveDockLayout();
        const UI::Rect before = view;
        ui.ClearDockLayout();
        harness.Move({640.0f, 400.0f}, describe);
        CHECK(view.min.x == 0.0f);
        CHECK(ui.LoadDockLayout(saved));
        harness.Move({640.0f, 400.0f}, describe);
        harness.Move({640.0f, 400.0f}, describe);
        CHECK(view.min.x == before.min.x && view.max.x == before.max.x);
        CHECK(!aOpen && bOpen);

        // Texte invalide : refusé, disposition inchangée.
        CHECK(!ui.LoadDockLayout("pas du json"));
        CHECK(!ui.LoadDockLayout(R"({"format":"flux.dock","version":1,"root":{"panels":[]}})")); // pas de zone centrale
        harness.Move({640.0f, 400.0f}, describe);
        CHECK(view.min.x == before.min.x);

        // Glisser-déposer : bouton enfoncé, déplacement en quelques frames, relâché.
        const auto drag = [&](Vector2f from, Vector2f to) {
            harness.Move(from, describe);
            harness.Frame([&] { harness.GetInput().SetMouseButton(MouseButton::Left, true); }, describe);
            for (int step = 1; step <= 4; ++step) {
                const float t = static_cast<float>(step) / 4.0f;
                harness.Move(from + (to - from) * t, describe);
            }
            harness.Frame([&] { harness.GetInput().SetMouseButton(MouseButton::Left, false); }, describe);
            harness.Move(to, describe);
        };
        const auto center = [](const UI::Rect& rect) { return (rect.min + rect.max) * 0.5f; };

        // L'onglet B sort de sa zone et reste flottant (relâché loin des cibles) ; A reprend la zone.
        const float rightMin = view.max.x + 4.0f;
        const Vector2f tabB{rightMin + 2.0f + tabAWidth + 2.0f + 10.0f, tabY};
        drag(tabB, {view.min.x + 40.0f, view.max.y - 40.0f});
        CHECK(aOpen && bOpen);
        CHECK(view.max.x == rightMin - 4.0f); // la zone de droite reste, avec A

        // A, dernier onglet de sa zone, glissé au centre de la zone de gauche : la zone de droite disparaît,
        // et A devient l'onglet affiché à gauche.
        const Vector2f rightTabA{rightMin + 10.0f, tabY};
        const Vector2f leftCenter{(0.0f + (view.min.x - 4.0f)) * 0.5f, 360.0f};
        drag(rightTabA, leftCenter);
        CHECK(view.max.x == 1280.0f);
        CHECK(aOpen && !leftOpen);

        // A, glissé sur la cible gauche de la vue : une nouvelle zone d'un quart de la vue.
        const float viewWidth = view.max.x - view.min.x;
        const Vector2f viewLeftTarget = center(view) - Vector2f{36.0f, 0.0f};
        drag({10.0f + ui.MeasureText("Gauche").x + 24.0f, tabY}, viewLeftTarget); // second onglet de gauche : A
        CHECK(aOpen && leftOpen);
        CHECK(std::abs((view.max.x - view.min.x) - (viewWidth - 4.0f) * 0.75f) <= 2.0f);

        // La disposition modifiée s'enregistre et se recharge.
        const std::string moved = ui.SaveDockLayout();
        CHECK(ui.LoadDockLayout(moved));
        CHECK(moved.find("\"A\"") != std::string::npos);
        return true;
    }

    bool RunRotationTests() {
        // ToYawPitchRoll inverse FromYawPitchRoll, hors verticale.
        const float angles[][3] = {{0.3f, 0.2f, -0.5f}, {-2.5f, 1.2f, 3.0f}, {1.0f, -0.7f, 0.0f}, {0.0f, 0.0f, 0.0f}};
        for (const auto& a : angles) {
            const Math::Quaternionf q = Math::Quaternionf::FromYawPitchRoll(a[0], a[1], a[2]);
            const Math::Vector3f back = Math::ToYawPitchRoll(q);
            const Math::Quaternionf again = Math::Quaternionf::FromYawPitchRoll(back.x, back.y, back.z);
            CHECK(std::abs(back.x - a[0]) < 1e-4f && std::abs(back.y - a[1]) < 1e-4f && std::abs(back.z - a[2]) < 1e-4f);
            CHECK(std::abs(q.x * again.x + q.y * again.y + q.z * again.z + q.w * again.w) > 0.99999f);
        }
        // À la verticale : même rotation, roulis reporté sur le lacet.
        const Math::Quaternionf vertical = Math::Quaternionf::FromYawPitchRoll(0.4f, 1.5707963f, 0.3f);
        const Math::Vector3f back = Math::ToYawPitchRoll(vertical);
        const Math::Quaternionf again = Math::Quaternionf::FromYawPitchRoll(back.x, back.y, back.z);
        CHECK(std::abs(vertical.x * again.x + vertical.y * again.y + vertical.z * again.z + vertical.w * again.w) > 0.9999f);
        return true;
    }
}

bool RunUiTests() {
    auto font = UI::Font::Load(Platform::GetExecutableDirectory() / "assets" / "fonts" / "Arimo.ttf");
    if (!font) {
        std::printf("ÉCHEC : police introuvable (%s)\n", font.error().c_str());
        return false;
    }
    return RunRotationTests() && RunWidgetTests(**font) && RunTreeTests(**font) && RunMenuTests(**font)
        && RunChoiceTests(**font) && RunDockTests(**font);
}
