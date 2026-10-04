#pragma once

#include "Flux/Export.hpp"
#include "Flux/Platform/Input.hpp"

#include <array>
#include <expected>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Actions d'entrée : le jeu demande « Sauter » plutôt que la touche Espace, et le joueur choisit ses touches.
// Chaque action a deux touches (principale et secondaire), et des touches par défaut. Les touches choisies par le
// joueur sont gardées par nom d'action, même pour une action pas encore déclarée (module de jeu rechargé).
namespace Flux {
    inline constexpr std::size_t InputActionKeyCount = 2;
    using InputActionKeys = std::array<Key, InputActionKeyCount>;

    struct InputAction
    {
        std::string name;     // « Avancer » : ce que le code demande, et ce que le joueur lit
        std::string category; // « Déplacement », « Vaisseau » : pour ranger l'écran des touches
        InputActionKeys keys{};
        InputActionKeys defaults{};
    };

    class FLUX_API InputActions {
        public:
        // Déclare une action, ou change ses touches par défaut si elle existe (les touches du joueur restent).
        void Add(std::string name, std::string category, Key primary, Key secondary = Key::Unknown);
        void Remove(std::string_view name);
        const InputAction* Find(std::string_view name) const;
        const std::vector<InputAction>& GetActions() const { return m_actions; }

        // Une des touches de l'action est enfoncée, ou vient de l'être (false pour une action inconnue).
        bool IsDown(const Input& input, std::string_view name) const;
        bool WasPressed(const Input& input, std::string_view name) const;
        // +1 si positive est tenue, -1 si negative l'est, 0 pour les deux ou aucune.
        float GetAxis(const Input& input, std::string_view positive, std::string_view negative) const;

        // Touche choisie par le joueur (slot 0 : principale, 1 : secondaire ; Key::Unknown l'efface).
        bool SetKey(std::string_view name, std::size_t slot, Key key);
        void ResetToDefaults();
        // Touches de l'action, lisibles : « Z / Haut » ; « (aucune) » sans touche.
        std::string GetKeysText(std::string_view name) const;

        // Fichier des touches choisies par le joueur (JSON « flux.input »), seulement celles qui diffèrent des défauts.
        std::string SaveToString() const;
        std::expected<void, std::string> LoadFromString(std::string_view text);

        // Nom des touches (GetKeyName par défaut ; les tests en donnent un sans Windows).
        void SetKeyNamer(std::function<std::string(Key)> namer) { m_namer = std::move(namer); }

        private:
        InputAction* FindMutable(std::string_view name);

        std::vector<InputAction> m_actions; // dans l'ordre de déclaration
        std::map<std::string, InputActionKeys, std::less<>> m_chosen; // touches du joueur, par nom d'action
        std::function<std::string(Key)> m_namer;
    };
}
