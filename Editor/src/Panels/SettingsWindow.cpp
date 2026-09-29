#include "Panels/SettingsWindow.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Graphics/Renderer.hpp>
#include <Flux/UI/Context.hpp>

namespace FluxEditor {
    using namespace Flux;

    void SettingsWindow::Draw(Engine& engine) {
        if (!m_open) {
            return;
        }
        UI::Context& ui = engine.GetUi();
        const Math::Vector2i screen = engine.GetWindow().GetSize();
        const float width = 420.0f;
        // Au milieu de l'écran à la première ouverture ; ensuite là où on l'a laissée.
        if (ui.BeginPanel("Paramètres", {(static_cast<float>(screen.x) - width) * 0.5f, static_cast<float>(screen.y) * 0.25f},
                          width)) {
            if (ui.Tab("Graphiques", m_tab == Tab::Graphics)) {
                m_tab = Tab::Graphics;
            }
            ui.Separator();

            if (m_tab == Tab::Graphics) {
                Renderer& renderer = engine.GetRenderer();
                ui.LabelText("GPU", renderer.GetDeviceName());
                bool vsync = renderer.IsVSyncEnabled();
                if (ui.Checkbox("V-Sync", vsync)) {
                    renderer.SetVSync(vsync);
                }
                ui.LabelText("Présentation", renderer.GetPresentModeName());
            }

            ui.Separator();
            if (ui.Button("Fermer")) {
                m_open = false;
            }
        }
        ui.EndPanel();
    }
}
