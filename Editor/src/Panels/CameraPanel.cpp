#include "Panels/CameraPanel.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/UI/Context.hpp>

namespace FluxEditor {
    void DrawCameraPanel(Flux::Engine& engine) {
        Flux::UI::Context& ui = engine.GetUi();
        ui.SetNextPanelIcon(Flux::UI::Icon::Camera);
        if (ui.BeginPanel("Caméra", {8.0f, 8.0f}, 360.0f)) {
            engine.GetCameraController().DrawFields(ui, engine.GetCamera());
        }
        ui.EndPanel();
    }
}
