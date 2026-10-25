#pragma once

// Caméra du joueur pendant le jeu : derrière le personnage (molette : distance, puis vue à la première personne ;
// clic droit : tourner autour), et aux commandes d'un vaisseau, vue cockpit ou vue extérieure tournée avec lui (V ou
// molette). Aux commandes, la souris dirige le vaisseau (curseur caché ; Alt maintenu ou un menu de pause le libère).

#include "Flux/Math/Quaternion.hpp"
#include "Flux/Math/Vector3.hpp"
#include "Flux/Scene/Components.hpp"

namespace Flux {
    struct Camera;
    class Engine;
    class Scene;

    class PlayerCamera {
        public:
        // Souris et molette, avant la mise à jour du jeu (rotation demandée au vaisseau piloté : GetSteerYaw...).
        void HandleInput(Engine& engine, Entity player);
        // Pas de personnage à suivre : le clic droit ne tourne plus autour de lui.
        void StopRotating() { m_followRotating = false; }
        // Après la physique et le jeu : la caméra voit le personnage là où il est dessiné.
        void Place(Engine& engine, Entity player, double deltaSeconds);
        // Suivi arrêté : la prochaine fois, la caméra part de là où elle est.
        void Reset();
        // Caméra replacée d'un coup derrière le personnage (chargement d'une partie).
        void Snap() { m_followShown = -1.0f; }

        float GetSteerYaw() const { return m_steerYaw; }
        float GetSteerPitch() const { return m_steerPitch; }
        void ToggleCockpitView() { m_cockpitView = !m_cockpitView; }
        // Avant et droite de la caméra, ramenés à l'horizontale de l'endroit (sur une planète : tangente au sol).
        Math::Vector3f GetWalkForward() const;
        Math::Vector3f GetWalkRight() const;
        // Personnage à ne pas dessiner (caméra dans sa tête : vue cockpit, première personne, coincée contre un mur).
        Entity GetHiddenPlayer() const { return m_hiddenPlayer; }

        private:
        // Poste de pilotage d'où la caméra regarde (vue cockpit), NullEntity si elle suit le personnage.
        Entity GetCockpitSeat(const Scene& scene, Entity player) const;
        void TurnWithShip(Engine& engine, Entity player);
        // Lacet et tangage de la caméra suiveuse d'après où regarde la caméra (début du suivi, retour du cockpit).
        void Aim(const Camera& camera);
        // Vue extérieure d'un vaisseau piloté depuis seat ; calcule sa boîte (m_chaseCenter, m_chaseRadius).
        void PlaceChase(Engine& engine, Entity seat, double deltaSeconds);
        void MeasureShip(Engine& engine, Entity ship);

        // Derrière le personnage, à cette distance (molette). Molette vers l'avant au plus près : vue à la première
        // personne (depuis ses yeux, personnage non dessiné) ; vers l'arrière : retour.
        float m_followDistance = 4.0f;
        bool m_firstPerson = false;
        float m_followShown = -1.0f; // distance réelle, raccourcie par les murs ; négative : pas encore placée
        Entity m_hiddenPlayer = NullEntity;
        bool m_followRotating = false; // clic droit maintenu : la souris tourne autour du personnage
        // Rotation du vaisseau demandée à cette image, en radians.
        float m_steerYaw = 0.0f;
        float m_steerPitch = 0.0f;
        // Vaisseau où se trouve le joueur, et sa rotation à la dernière image : la caméra tourne avec lui.
        Entity m_followShip = NullEntity;
        Math::Quaternionf m_followShipRotation;
        // Repère où la caméra qui suit le joueur tourne (lacet, tangage) : son y est le haut de l'endroit (le monde, le
        // pont du vaisseau, ou la verticale de la planète), son horizontale celle du joueur. Il tourne avec le
        // vaisseau et se redresse au plus court quand le haut change (on fait le tour d'une planète).
        Math::Quaternionf m_followFrame;
        bool m_followFrameSet = false;
        float m_followYaw = 0.0f;
        float m_followPitch = 0.0f;
        // Aux commandes : vue depuis l'œil du pilote, ou de l'extérieur, et où il regarde (clic droit).
        bool m_cockpitView = true;
        float m_lookYaw = 0.0f;
        float m_lookPitch = 0.0f;
        // Vue extérieure : distance en tailles du vaisseau (molette), et sa boîte (centre dans son repère, rayon),
        // calculée une fois par vaisseau piloté.
        float m_chaseZoom = 1.4f;
        float m_chaseShown = -1.0f; // distance réelle, raccourcie par les murs ; négative : pas encore placée
        Entity m_chaseShip = NullEntity;
        Math::Vector3f m_chaseCenter;
        float m_chaseRadius = 10.0f;
    };
}
