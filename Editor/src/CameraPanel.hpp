#pragma once

namespace Flux {
    class Engine;
}

namespace FluxEditor {
    // Panneau « Caméra » : position de la caméra de l'éditeur, vitesse de vol, rappel des commandes.
    void DrawCameraPanel(Flux::Engine& engine);
}
