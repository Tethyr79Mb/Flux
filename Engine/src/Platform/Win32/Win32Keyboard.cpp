#include "Flux/Platform/Input.hpp"

#include "Platform/Win32/Win32Utf.hpp"

#include <array>

namespace Flux {
    std::string GetKeyName(Key key) {
        const auto scancode = static_cast<unsigned>(key);
        if (scancode == 0) {
            return {};
        }
        switch (key) {
            case Key::MouseLeft: return "Clic gauche";
            case Key::MouseRight: return "Clic droit";
            case Key::MouseMiddle: return "Clic milieu";
            default: break;
        }
        // Scancode dans les bits 16 à 23, bit 24 pour les touches étendues (préfixe 0xE0).
        const LONG param = static_cast<LONG>(((scancode & 0xFFu) << 16) | ((scancode & 0x100u) ? (1u << 24) : 0u));
        std::array<wchar_t, 64> name{};
        const int length = GetKeyNameTextW(param, name.data(), static_cast<int>(name.size()));
        if (length <= 0) {
            return "Touche " + std::to_string(scancode);
        }
        return Win32::ToUtf8({name.data(), static_cast<std::size_t>(length)});
    }
}
