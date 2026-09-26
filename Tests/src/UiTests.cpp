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
    return RunRotationTests() && RunWidgetTests(**font) && RunTreeTests(**font);
}
