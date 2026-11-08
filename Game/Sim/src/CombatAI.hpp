#pragma once

#include <Flux/Math/WorldPosition.hpp>
#include <Flux/Scene/Components.hpp>
#include "Weapons.hpp"

#include <cstdint>
#include <vector>

namespace Flux {
    class ComponentRegistry;
    class Scene;
}

// IA de combat des ennemis (sortie du moteur le 2026-11-01).
namespace Game {
    // État d'un vaisseau mené par l'IA de combat (CombatAI).
    enum class CombatState : std::uint8_t {
        Patrol, // tourne autour de son point de départ, en attendant de voir la cible
        Attack, // fonce sur elle et tire quand elle est devant lui
        Break,  // rompt le combat un instant (passé trop près, ou touché s'il est prudent), puis revient
        Flee,   // presque détruit : s'enfuit à toute vitesse
    };

    // IA de combat d'un vaisseau ennemi (CombatAI) : il attaque le vaisseau du joueur avec ses armes (emports). Les
    // réglages font des pirates prudents ou kamikazes sans code.
    struct CombatAIComponent
    {
        float detectionRange = 6000.0f; // m : il voit la cible et l'attaque ; à une fois et demie, il la perd
        float preferredRange = 800.0f;  // m : distance de combat ; plus loin il fonce, plus près il ralentit
        // 0 : prudent (rompt tôt, rompt quand il est touché, tire peu de missiles) ; 1 : kamikaze (colle à la cible).
        float aggression = 0.5f;
        float inaccuracy = 1.0f;    // °, erreur de visée qui va et vient (1° : 17 m à 1 km)
        float fleeHealth = 20.0f;   // % de ses points de vie de départ : en dessous, il fuit
        float patrolRadius = 300.0f; // m, cercle autour de son point de départ
        // Pendant le jeu (non enregistrés).
        CombatState state = CombatState::Patrol;
        float stateTime = 0.0f;      // s dans l'état
        float clock = 0.0f;          // s depuis le début : fait varier l'erreur de visée
        float startHealth = -1.0f;   // points de vie au premier passage (-1 : pas encore lus)
        float lastHealth = 0.0f;     // à l'image d'avant : touché s'ils baissent
        float missileCooldown = 0.0f; // s avant de pouvoir lancer un missile
        bool firing = false;         // la cible est devant lui : il tire (CombatAI::Fire)
        bool hasHome = false;
        Flux::Math::WorldPosition home;
        Flux::Math::Vector3f breakDirection; // monde : où il file en rompant
    };

    // Enregistre CombatAIComponent (« combatAI », même nom qu'au temps du moteur : les scènes se relisent).
    void RegisterCombatAI(Flux::ComponentRegistry& registry);

    // IA de combat des vaisseaux ennemis (CombatAIComponent) : une machine à états (patrouille, attaque, rupture,
    // fuite) qui mène le vaisseau comme le pilote automatique (vitesse et rotation de ShipComponent) et tire avec ses
    // armes (Weapons::Update). Seulement les vaisseaux à la racine de la scène, hors warp et pilote automatique.
    namespace CombatAI {
        // Choix de l'état et du cap. target : le vaisseau du joueur (NullEntity : aucun, ils patrouillent). Pendant le
        // jeu, avant la physique (comme Ships::Update).
        void Steer(Flux::Scene& scene, double deltaSeconds, Flux::Entity target);

        // Tir de ceux qui ont la cible devant eux ; après la physique, comme les armes du joueur.
        void Fire(Flux::Scene& scene, double deltaSeconds, Flux::Entity target, std::vector<Flux::Projectile>& projectiles);
    }
}
