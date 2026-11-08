#pragma once

#include <Flux/Math/WorldPosition.hpp>
#include <Flux/Scene/Components.hpp>

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Flux {
    class Scene;
    class Simulation;
}

// Missions du jeu : parler à un donneur (Interagir), accepter, suivre le groupe d'ennemis jusqu'à sa destruction. Les
// briques (MissionGiverComponent, Missions::SpawnEnemies...) sont dans MissionGiver.hpp ; le déroulement est ici, la
// fenêtre et les indications à l'écran au client (MissionsHud.cpp).
namespace Game {
    struct SimState;

    // Mission acceptée auprès d'un donneur, une seule à la fois : ses ennemis encore à détruire.
    struct ActiveMission
    {
        Flux::Entity giver = Flux::NullEntity;
        std::string title;
        std::vector<Flux::Entity> enemies;
        Flux::Math::WorldPosition area; // où ils sont apparus : destination du warp
    };

    struct MissionState
    {
        std::optional<ActiveMission> mission;
        Flux::Entity window = Flux::NullEntity; // donneur auquel le joueur parle (fenêtre ouverte)
        std::string message;                    // « Mission accomplie », en haut de l'écran
        double messageSeconds = 0.0;            // temps d'affichage restant, décompté par l'affichage
    };

    namespace Missions {
        inline constexpr double MessageSeconds = 5.0;

        // Étape Interaction : mission accomplie quand tous ses ennemis sont détruits ; Interagir près d'un donneur
        // ouvre ou ferme la discussion.
        void Interact(Flux::Simulation& simulation, SimState& state);
        // Accepte la mission du donneur : son groupe d'ennemis apparaît. En cas d'échec, rien ne change.
        std::expected<void, std::string> Accept(Flux::Simulation& simulation, SimState& state, Flux::Entity giver);
        // Abandonne la mission en cours : son groupe repart avec elle.
        void Abandon(Flux::Simulation& simulation, SimState& state);
        void Reset(MissionState& state);
        // Sauvegarde rapide : le donneur puis les ennemis encore en vie, par identifiant ; vide sans mission.
        std::string Save(const Flux::Scene& scene, const MissionState& state);
        void Load(Flux::Simulation& simulation, SimState& state, std::string_view data);
    }
}
