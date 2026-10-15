#include "Flux/Platform/Input.hpp"

namespace Flux {
    void Input::BeginFrame() {
        m_mouseDelta = {};
        m_wheelDelta = 0.0f;
        m_pressed.fill(false);
        m_typed.fill(false);
        m_letterPressed.fill(false);
        m_buttonPressed.fill(false);
        m_buttonReleased.fill(false);
        m_textInput.clear();
    }

    void Input::SetKey(std::uint16_t scancode, bool down) {
        if (scancode < KeyCount) {
            if (down && !m_keys[scancode]) {
                m_pressed[scancode] = true;
            }
            if (down) {
                m_typed[scancode] = true; // premier appui et répétitions
            }
            m_keys[scancode] = down;
        }
    }

    Key Input::GetFirstPressedKey() const {
        for (std::size_t scancode = 1; scancode < KeyCount; ++scancode) {
            if (m_pressed[scancode]) {
                return static_cast<Key>(scancode);
            }
        }
        return Key::Unknown;
    }

    void Input::SetLetterPressed(char letter) {
        if (letter >= 'A' && letter <= 'Z') {
            m_letterPressed[static_cast<std::size_t>(letter - 'A')] = true;
        }
    }

    void Input::SetMouseButton(MouseButton button, bool down) {
        const std::size_t index = Index(button);
        if (down && !m_buttons[index]) {
            m_buttonPressed[index] = true;
        }
        if (!down && m_buttons[index]) {
            m_buttonReleased[index] = true;
        }
        m_buttons[index] = down;
        SetKey(static_cast<std::uint16_t>(static_cast<std::uint16_t>(Key::MouseLeft) + index), down); // voir Key::MouseLeft
    }

    void Input::AddMouseDelta(float dx, float dy) {
        m_mouseDelta.x += dx;
        m_mouseDelta.y += dy;
    }

    void Input::AddWheelDelta(float notches) {
        m_wheelDelta += notches;
    }

    void Input::AddCharacter(char32_t codepoint) {
        if (codepoint < 0x20 || codepoint == 0x7F || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            return; // contrôle ou point de code invalide
        }
        const auto put = [&](unsigned value) { m_textInput.push_back(static_cast<char>(value)); };
        if (codepoint < 0x80) {
            put(codepoint);
        } else if (codepoint < 0x800) {
            put(0xC0 | (codepoint >> 6));
            put(0x80 | (codepoint & 0x3F));
        } else if (codepoint < 0x10000) {
            put(0xE0 | (codepoint >> 12));
            put(0x80 | ((codepoint >> 6) & 0x3F));
            put(0x80 | (codepoint & 0x3F));
        } else {
            put(0xF0 | (codepoint >> 18));
            put(0x80 | ((codepoint >> 12) & 0x3F));
            put(0x80 | ((codepoint >> 6) & 0x3F));
            put(0x80 | (codepoint & 0x3F));
        }
    }

    void Input::ReleaseAll() {
        m_keys.fill(false);
        m_pressed.fill(false);
        m_typed.fill(false);
        m_letterPressed.fill(false);
        m_buttons.fill(false);
        m_buttonPressed.fill(false);
        m_buttonReleased.fill(false);
    }
}
