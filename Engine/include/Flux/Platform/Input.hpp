#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Vector2.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Flux {
    // Touches repérées par leur position physique (scancodes PC), pas par la lettre imprimée :
    // les noms suivent le clavier QWERTY, donc Key::W est la touche Z d'un clavier AZERTY.
    // Ainsi ZQSD (AZERTY) et WASD (QWERTY) tombent sous les mêmes doigts sans rien configurer.
    enum class Key : std::uint16_t {
        Unknown = 0,
        Escape = 0x01,
        Digit1 = 0x02, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9, Digit0,
        Backspace = 0x0E,
        Tab = 0x0F,
        Q = 0x10, W, E, R, T, Y, U, I, O, P,
        Enter = 0x1C,
        LeftControl = 0x1D,
        A = 0x1E, S, D, F, G, H, J, K, L,
        LeftShift = 0x2A,
        Z = 0x2C, X, C, V, B, N, M,
        LeftAlt = 0x38,
        Space = 0x39,
        F1 = 0x3B, F2, F3, F4, F5, F6, F7, F8, F9, F10,
        F11 = 0x57, F12,
        // Touches étendues : scancode préfixé par 0xE0.
        NumpadEnter = 0x11C,
        RightControl = 0x11D,
        RightAlt = 0x138,
        Home = 0x147,
        Up = 0x148,
        Left = 0x14B,
        Right = 0x14D,
        End = 0x14F,
        Down = 0x150,
        Delete = 0x153,
        // Boutons de la souris, rangés après les scancodes du clavier : une action d'entrée (InputActions) peut
        // ainsi être un clic (une action du jeu au clic gauche) et se réassigner comme une touche.
        MouseLeft = 0x1F0,
        MouseRight,
        MouseMiddle,
    };

    enum class MouseButton : std::uint8_t { Left, Right, Middle, Count };

    // Forme du curseur de la souris (Window::SetMouseCursor) : flèche, ou double flèche au bord d'une fenêtre.
    enum class MouseCursor : std::uint8_t {
        Arrow,
        ResizeHorizontal, // ↔ : bord gauche ou droit, séparateur vertical
        ResizeVertical,   // ↕ : bord haut ou bas
        ResizeNwse,       // ⤡ : coins haut-gauche et bas-droit
        ResizeNesw,       // ⤢ : coins haut-droit et bas-gauche
        Move,             // ✥ : élément qu'on peut glisser
    };

    // Nom d'une touche selon la disposition du clavier, comme Windows l'écrit (Key::W donne « Z » en AZERTY,
    // Key::Space « ESPACE ») ; vide pour Key::Unknown.
    FLUX_API std::string GetKeyName(Key key);

    // État du clavier et de la souris, mis à jour par Window::HandleEvents.
    class FLUX_API Input {
        public:
        static constexpr std::size_t KeyCount = 0x200;

        bool IsKeyDown(Key key) const { return m_keys[static_cast<std::size_t>(key) % KeyCount]; }
        // Vrai seulement à la frame où la touche vient d'être enfoncée (répétition automatique ignorée).
        bool WasKeyPressed(Key key) const { return m_pressed[static_cast<std::size_t>(key) % KeyCount]; }
        // Comme WasKeyPressed, mais compte aussi la répétition automatique (touche maintenue) : pour l'édition de texte.
        bool WasKeyTyped(Key key) const { return m_typed[static_cast<std::size_t>(key) % KeyCount]; }
        // Vrai à la frame où la touche portant cette lettre (A à Z) vient d'être enfoncée, selon la disposition du
        // clavier : pour les raccourcis qui se lisent par leur lettre (Ctrl+Z annule aussi en AZERTY).
        bool WasLetterPressed(char letter) const {
            return letter >= 'A' && letter <= 'Z' && m_letterPressed[static_cast<std::size_t>(letter - 'A')];
        }
        // Première touche enfoncée à cette frame (réassignation d'une action) ; Key::Unknown si aucune.
        Key GetFirstPressedKey() const;
        bool IsControlDown() const { return IsKeyDown(Key::LeftControl) || IsKeyDown(Key::RightControl); }
        bool IsShiftDown() const { return IsKeyDown(Key::LeftShift); }

        bool IsMouseButtonDown(MouseButton button) const { return m_buttons[Index(button)]; }
        // Vrai seulement à la frame où le bouton vient d'être enfoncé, ou relâché.
        bool WasMouseButtonPressed(MouseButton button) const { return m_buttonPressed[Index(button)]; }
        bool WasMouseButtonReleased(MouseButton button) const { return m_buttonReleased[Index(button)]; }

        // Position du curseur dans la zone client, en pixels depuis le coin haut-gauche.
        Math::Vector2f GetMousePosition() const { return m_mousePosition; }

        // Déplacement brut de la souris depuis la frame précédente, en unités du capteur (sans accélération Windows).
        Math::Vector2f GetMouseDelta() const { return m_mouseDelta; }
        // Crans de molette depuis la frame précédente, positif vers l'avant.
        float GetWheelDelta() const { return m_wheelDelta; }

        // Caractères tapés depuis la frame précédente, en UTF-8, selon la disposition du clavier (AZERTY compris).
        // Les caractères de contrôle (retour arrière, entrée...) n'y sont pas : voir WasKeyTyped.
        std::string_view GetTextInput() const { return m_textInput; }

        // Utilisés par la couche plateforme.
        void BeginFrame();
        void SetKey(std::uint16_t scancode, bool down);
        void SetMouseButton(MouseButton button, bool down);
        void SetLetterPressed(char letter);
        void SetMousePosition(Math::Vector2f position) { m_mousePosition = position; }
        void AddMouseDelta(float dx, float dy);
        void AddCharacter(char32_t codepoint);
        void AddWheelDelta(float notches);
        // Perte du focus : on ne recevra pas les relâchements, donc tout est relâché.
        void ReleaseAll();

        private:
        static constexpr std::size_t ButtonCount = static_cast<std::size_t>(MouseButton::Count);
        static constexpr std::size_t Index(MouseButton button) { return static_cast<std::size_t>(button) % ButtonCount; }

        std::array<bool, KeyCount> m_keys{};
        std::array<bool, KeyCount> m_pressed{};
        std::array<bool, KeyCount> m_typed{};
        std::array<bool, 26> m_letterPressed{};
        std::array<bool, ButtonCount> m_buttons{};
        std::array<bool, ButtonCount> m_buttonPressed{};
        std::array<bool, ButtonCount> m_buttonReleased{};
        Math::Vector2f m_mousePosition;
        Math::Vector2f m_mouseDelta;
        float m_wheelDelta = 0.0f;
        std::string m_textInput;
    };
}
