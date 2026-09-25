#include "Flux/Platform/Input.hpp"

namespace Flux {
    void Input::BeginFrame() {
        m_mouseDelta = {};
        m_wheelDelta = 0.0f;
        m_pressed.fill(false);
    }

    void Input::SetKey(std::uint16_t scancode, bool down) {
        if (scancode < KeyCount) {
            if (down && !m_keys[scancode]) {
                m_pressed[scancode] = true;
            }
            m_keys[scancode] = down;
        }
    }

    void Input::SetMouseButton(MouseButton button, bool down) {
        m_buttons[static_cast<std::size_t>(button)] = down;
    }

    void Input::AddMouseDelta(float dx, float dy) {
        m_mouseDelta.x += dx;
        m_mouseDelta.y += dy;
    }

    void Input::AddWheelDelta(float notches) {
        m_wheelDelta += notches;
    }

    void Input::ReleaseAll() {
        m_keys.fill(false);
        m_pressed.fill(false);
        m_buttons.fill(false);
    }
}
