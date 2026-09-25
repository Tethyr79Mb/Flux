#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Vector2.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace Flux {
    // Touches repérées par leur position physique (scancodes PC), pas par la lettre imprimée :
    // les noms suivent le clavier QWERTY, donc Key::W est la touche Z d'un clavier AZERTY.
    // Ainsi ZQSD (AZERTY) et WASD (QWERTY) tombent sous les mêmes doigts sans rien configurer.
    enum class Key : std::uint16_t {
        Unknown = 0,
        Escape = 0x01,
        Digit1 = 0x02, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9, Digit0,
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
        RightControl = 0x11D,
        RightAlt = 0x138,
        Up = 0x148,
        Left = 0x14B,
        Right = 0x14D,
        Down = 0x150,
    };

    enum class MouseButton : std::uint8_t { Left, Right, Middle, Count };

    // État du clavier et de la souris, mis à jour par Window::HandleEvents.
    class FLUX_API Input {
        public:
        static constexpr std::size_t KeyCount = 0x200;

        bool IsKeyDown(Key key) const { return m_keys[static_cast<std::size_t>(key) % KeyCount]; }
        bool IsMouseButtonDown(MouseButton button) const { return m_buttons[static_cast<std::size_t>(button)]; }

        // Déplacement brut de la souris depuis la frame précédente, en unités du capteur (sans accélération Windows).
        Math::Vector2f GetMouseDelta() const { return m_mouseDelta; }
        // Crans de molette depuis la frame précédente, positif vers l'avant.
        float GetWheelDelta() const { return m_wheelDelta; }

        // Utilisés par la couche plateforme.
        void BeginFrame();
        void SetKey(std::uint16_t scancode, bool down);
        void SetMouseButton(MouseButton button, bool down);
        void AddMouseDelta(float dx, float dy);
        void AddWheelDelta(float notches);
        // Perte du focus : on ne recevra pas les relâchements, donc tout est relâché.
        void ReleaseAll();

        private:
        std::array<bool, KeyCount> m_keys{};
        std::array<bool, static_cast<std::size_t>(MouseButton::Count)> m_buttons{};
        Math::Vector2f m_mouseDelta;
        float m_wheelDelta = 0.0f;
    };
}
