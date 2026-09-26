#pragma once

#include "Flux/Export.hpp"

namespace Flux {
    struct Camera;
    class Window;

    namespace UI {
        class Context;
    }

    struct FlyCameraSettings
    {
        float lookSensitivity = 0.0025f; // radians par unité de déplacement brut de la souris
        double speed = 10.0;             // m/s au départ
        double minSpeed = 0.01;
        double maxSpeed = 1.0e15;        // de quoi traverser un système solaire en quelques secondes
        double wheelFactor = 1.5;        // vitesse multipliée ou divisée par cran de molette
        double boostFactor = 5.0;        // Maj enfoncée
    };

    // Vol libre façon Unreal : clic droit maintenu pour piloter, souris pour regarder,
    // ZQSD (WASD en QWERTY) pour avancer, E / A (E / Q en QWERTY) pour monter / descendre, molette pour la vitesse.
    class FLUX_API FlyCameraController {
        public:
        explicit FlyCameraController(const FlyCameraSettings& settings = {});

        // deltaSeconds : durée de la frame précédente.
        // mouseAvailable false (souris sur l'interface) : un clic droit ne prend pas les commandes.
        // keyboardAvailable false (saisie dans un champ) : les touches ne déplacent pas la caméra.
        void Update(Camera& camera, Window& window, double deltaSeconds, bool mouseAvailable = true,
                    bool keyboardAvailable = true);

        // Champs FluxUI (dans un panneau ouvert) : secteur, position et vitesse, puis l'aide des commandes.
        void DrawFields(UI::Context& ui, Camera& camera);

        bool IsActive() const { return m_active; }
        double GetSpeed() const { return m_speed; }
        void SetSpeed(double speed);

        private:
        FlyCameraSettings m_settings;
        double m_speed;
        bool m_active = false;
    };
}
