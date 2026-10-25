#pragma once

// Indications du moteur à l'écran pendant le jeu : infos de vol du vaisseau piloté (vitesse, altitude, air, quai),
// leur message bref et les lignes du jeu, et le message court en haut de l'écran (« Partie sauvegardée »).

#include "Flux/Math/Vector2.hpp"
#include "Flux/Math/Vector3.hpp"
#include "Flux/Math/WorldPosition.hpp"
#include "Flux/Scene/Components.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace Flux {
    class PhysicsWorld;
    class Scene;

    namespace Planetology {
        class Terrain;
    }

    namespace UI {
        class DrawList;
    }

    class FlightHud {
        public:
        // Où vole un vaisseau par rapport à la planète la plus proche : altitude au-dessus du sol (ou de la mer qui
        // le recouvre), verticale du lieu (unitaire, monde) et densité de l'air (0 dans l'espace, voir ShipInput::air).
        struct FlightState
        {
            bool nearPlanet = false;
            double altitude = 0.0;
            Math::Vector3d up;
            float air = 0.0f;
            double boundary = 0.0; // distance au haut de l'atmosphère, ou à l'altitude sans atmosphère (ShipInput::planetDistance)
            double center = 0.0;   // distance au centre de la planète (ShipInput::planetCenter)
        };
        // Garde le relief de la dernière planète : il n'est recréé que si elle change.
        FlightState GetFlightState(Scene& scene, PhysicsWorld* physics, Entity ship);

        // Vitesse, vitesse verticale et altitude du vaisseau piloté, en bas de l'écran.
        void DrawFlightInfo(Scene& scene, PhysicsWorld* physics, UI::DrawList& list, Entity player, Math::Vector2f screen,
                            double deltaSeconds);
        // Message bref sous les infos de vol (pilote automatique lancé, aucun quai à portée).
        void ShowFlightMessage(std::string message);
        // Lignes du jeu sous les infos de vol (Engine::SetFlightInfoExtra).
        void SetExtra(std::string_view text) { m_extra.assign(text); } // garde sa capacité : pas d'allocation
        void ClearExtra() { m_extra.clear(); }

        void ShowNotice(std::string text);
        bool HasNotice() const { return m_noticeSeconds > 0.0; }
        // loading : l'écran de chargement le cache, il attend la fin pour décompter.
        void DrawNotice(UI::DrawList& list, Math::Vector2f screen, double deltaSeconds, bool loading);

        private:
        std::shared_ptr<const Planetology::Terrain> m_terrain; // relief de la planète la plus proche
        std::int32_t m_terrainSeed = 0;
        float m_terrainWidth = 0.0f;
        // Vitesse réellement parcourue par le vaisseau piloté : sa position à l'image d'avant et la vitesse mesurée,
        // lissée.
        Entity m_ship = NullEntity;
        Math::WorldPosition m_position;
        Math::Vector3d m_velocity;
        std::string m_message;
        double m_messageSeconds = 0.0;
        std::string m_extra;
        std::string m_notice;
        double m_noticeSeconds = 0.0;
    };
}
