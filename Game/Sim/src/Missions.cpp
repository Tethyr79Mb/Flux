#include "Missions.hpp"

#include "GameSim.hpp"
#include "MissionGiver.hpp"

#include <Flux/Core/Log.hpp>
#include <Flux/Core/Simulation.hpp>
#include <Flux/Scene/Scene.hpp>

#include <sstream>

namespace Game::Missions {
    namespace {
        using namespace Flux;

        void SetMission(PlayerStateComponent& state, std::optional<ActiveMission> mission) {
            state.missions.mission = std::move(mission);
            const std::optional<ActiveMission>& current = state.missions.mission;
            Navigation::SetWarpObjective(state.navigation, current ? "Mission : " + current->title : std::string{},
                                         current ? std::optional{current->area} : std::nullopt);
        }
    }

    void Interact(Simulation& simulation) {
        Scene& scene = simulation.GetScene();
        for (const Entity player : simulation.GetPlayers()) {
            PlayerStateComponent& state = GetPlayerState(scene, player);
            MissionState& missions = state.missions;
            // Tous les ennemis détruits : mission accomplie.
            if (missions.mission && CountAlive(scene, missions.mission->enemies) == 0) {
                missions.message = "Mission accomplie : " + missions.mission->title;
                missions.messageSeconds = MessageSeconds;
                Log::Info("{}", missions.message);
                SetMission(state, std::nullopt);
            }
            if (scene.Get<CharacterComponent>(player).seat != NullEntity) {
                missions.window = NullEntity; // assis : Interagir sert au siège
                continue;
            }
            const Entity giver = FindGiver(scene, player);
            if (giver != NullEntity && simulation.ConsumeInteraction(player)) {
                missions.window = missions.window == giver ? NullEntity : giver; // encore : la referme
            } else if (missions.window != NullEntity && giver != missions.window) {
                missions.window = NullEntity; // parti, ou donneur disparu
            }
        }
    }

    std::expected<void, std::string> Accept(Simulation& simulation, Entity player, Entity giver) {
        Scene& scene = simulation.GetScene();
        const auto* component = scene.IsValid(giver) ? scene.TryGet<MissionGiverComponent>(giver) : nullptr;
        if (!component) {
            return std::unexpected("donneur introuvable");
        }
        if (player == NullEntity || !scene.IsValid(player)) {
            return std::unexpected("joueur introuvable");
        }
        PlayerStateComponent& state = GetPlayerState(scene, player);
        if (state.missions.mission) {
            return std::unexpected("une autre mission est déjà en cours");
        }
        auto enemies = SpawnEnemies(scene, simulation.GetComponents(), simulation.GetPrefabReader(), giver);
        if (!enemies) {
            Log::Warning("Mission « {} » : {}", component->title, enemies.error());
            state.missions.message = "Mission impossible : ennemis introuvables";
            state.missions.messageSeconds = MessageSeconds;
            return std::unexpected(enemies.error());
        }
        SetMission(state, ActiveMission{giver, component->title, std::move(*enemies), GetTargetArea(scene, giver)});
        Log::Info("Mission acceptée : {}", state.missions.mission->title);
        return {};
    }

    void Abandon(Simulation& simulation, Entity player) {
        Scene& scene = simulation.GetScene();
        PlayerStateComponent* state = FindPlayerState(scene, player);
        if (!state || !state->missions.mission) {
            return;
        }
        // Le groupe repart avec la mission : rien ne reste à abattre pour rien.
        for (const Entity enemy : state->missions.mission->enemies) {
            if (scene.IsValid(enemy)) {
                scene.DestroyEntity(enemy);
            }
        }
        SetMission(*state, std::nullopt);
    }

    std::string Save(const Scene& scene, const MissionState& state) {
        if (!state.mission || !scene.IsValid(state.mission->giver)) {
            return {};
        }
        // Titre et zone sont relus sur le donneur.
        std::string text = scene.GetId(state.mission->giver).ToString();
        for (const Entity enemy : state.mission->enemies) {
            if (scene.IsValid(enemy)) {
                text += ' ';
                text += scene.GetId(enemy).ToString();
            }
        }
        return text;
    }

    void Load(Simulation& simulation, Entity player, std::string_view data) {
        Scene& scene = simulation.GetScene();
        if (player == NullEntity || !scene.IsValid(player)) {
            return;
        }
        std::istringstream words{std::string(data)};
        std::string word;
        if (!(words >> word)) {
            return;
        }
        const std::optional<Uuid> giverId = Uuid::Parse(word);
        const Entity giver = giverId ? scene.FindById(*giverId) : NullEntity;
        const auto* component = giver != NullEntity ? scene.TryGet<MissionGiverComponent>(giver) : nullptr;
        if (!component) {
            Log::Warning("sauvegarde : donneur de la mission introuvable, mission oubliée");
            return;
        }
        ActiveMission mission{giver, component->title, {}, GetTargetArea(scene, giver)};
        while (words >> word) {
            if (const std::optional<Uuid> id = Uuid::Parse(word)) {
                if (const Entity enemy = scene.FindById(*id); enemy != NullEntity) {
                    mission.enemies.push_back(enemy);
                }
            }
        }
        SetMission(GetPlayerState(scene, player), std::move(mission)); // sans ennemi restant : accomplie au prochain pas
    }
}
