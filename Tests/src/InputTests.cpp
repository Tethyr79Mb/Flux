// Actions d'entrée (InputActions) : touches par défaut, touches du joueur, axes, sauvegarde.
#include "Check.hpp"

#include "Flux/Input/InputActions.hpp"
#include "Flux/Platform/Input.hpp"

#include <cstdint>
#include <string>

using namespace Flux;

namespace {
    // Noms sans Windows, pour des résultats identiques partout.
    std::string TestKeyName(Key key) {
        switch (key) {
        case Key::W: return "W";
        case Key::S: return "S";
        case Key::Up: return "Haut";
        case Key::Space: return "Espace";
        default: return "?";
        }
    }

    void Press(Input& input, Key key) { input.SetKey(static_cast<std::uint16_t>(key), true); }
}

bool RunInputTests() {
    InputActions actions;
    actions.SetKeyNamer(TestKeyName);
    actions.Add("Avancer", "Déplacement", Key::W, Key::Up);
    actions.Add("Reculer", "Déplacement", Key::S);
    actions.Add("Sauter", "Déplacement", Key::Space);

    CHECK(actions.GetActions().size() == 3);
    CHECK(actions.GetKeysText("Avancer") == "W / Haut");
    CHECK(actions.GetKeysText("Reculer") == "S");
    CHECK(actions.GetKeysText("Inconnue") == "(aucune)");

    // Touche secondaire et axe.
    Input input;
    input.BeginFrame();
    Press(input, Key::Up);
    CHECK(actions.IsDown(input, "Avancer"));
    CHECK(actions.WasPressed(input, "Avancer"));
    CHECK(!actions.IsDown(input, "Reculer"));
    CHECK(!actions.IsDown(input, "Inconnue"));
    CHECK(actions.GetAxis(input, "Avancer", "Reculer") == 1.0f);
    Press(input, Key::S);
    CHECK(actions.GetAxis(input, "Avancer", "Reculer") == 0.0f);

    // Touche du joueur : remplace la principale, et seule elle est sauvegardée.
    CHECK(actions.SetKey("Sauter", 0, Key::W));
    CHECK(!actions.SetKey("Inconnue", 0, Key::W));
    CHECK(actions.GetKeysText("Sauter") == "W");
    const std::string saved = actions.SaveToString();
    CHECK(saved.find("Sauter") != std::string::npos);
    CHECK(saved.find("Avancer") == std::string::npos);

    // Remettre la touche par défaut efface le choix.
    CHECK(actions.SetKey("Sauter", 0, Key::Space));
    CHECK(actions.SaveToString().find("Sauter") == std::string::npos);

    // Rechargement : le choix est gardé même pour une action déclarée après (module de jeu rechargé).
    InputActions loaded;
    loaded.SetKeyNamer(TestKeyName);
    CHECK(loaded.LoadFromString(saved).has_value());
    loaded.Add("Sauter", "Déplacement", Key::Space);
    CHECK(loaded.GetKeysText("Sauter") == "W");
    loaded.Remove("Sauter");
    CHECK(loaded.Find("Sauter") == nullptr);
    loaded.Add("Sauter", "Déplacement", Key::Space);
    CHECK(loaded.GetKeysText("Sauter") == "W");

    // Touches par défaut.
    loaded.ResetToDefaults();
    CHECK(loaded.GetKeysText("Sauter") == "Espace");
    CHECK(loaded.SaveToString().find("Sauter") == std::string::npos);

    // Fichier abîmé : erreur, sans rien changer.
    CHECK(!loaded.LoadFromString("pas du JSON").has_value());
    CHECK(!loaded.LoadFromString(R"({"format":"autre","version":1})").has_value());
    CHECK(loaded.GetKeysText("Sauter") == "Espace");
    return true;
}
