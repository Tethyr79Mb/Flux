// Passages entre scènes (PassageComponent) : le vaisseau piloté, avec ce qu'il porte, passe de l'extérieur d'une
// station à son hangar (une autre scène) et en revient. La scène quittée est gardée en mémoire telle quelle.
#include "Flux/Core/Engine.hpp"

#include "Flux/Audio/AudioSystem.hpp"
#include "Flux/Core/Log.hpp"
#include "Flux/Physics/PhysicsWorld.hpp"
#include "Flux/Platform/Dialogs.hpp"
#include "Flux/Platform/Filesystem.hpp"
#include "Flux/Scene/Docking.hpp"
#include "Core/LoadingScreen.hpp"
#include "Core/QuickSave.hpp"

#include <algorithm>
#include <cmath>

namespace Flux {
    namespace {
        // Racine de la scène à cette place du monde (secteur compris), orientée ainsi.
        void PlaceRoot(Scene& scene, Entity entity, const Math::WorldPosition& position, const Math::Quaternionf& rotation) {
            if (scene.GetParent(entity) != NullEntity) {
                scene.SetParent(entity, NullEntity);
            }
            TransformComponent& transform = scene.Get<TransformComponent>(entity);
            transform.position = position.offset;
            transform.rotation = rotation;
            if (position.sector != Math::Vector3i64{}) {
                scene.GetRegistry().emplace_or_replace<SectorComponent>(entity, position.sector);
            } else {
                scene.GetRegistry().remove<SectorComponent>(entity);
            }
        }

        std::string NameOf(const Scene& scene, Entity entity) {
            const auto* name = scene.TryGet<NameComponent>(entity);
            return name ? name->name : std::string("passage");
        }
    }

    void Engine::CheckPassages(Entity ship) {
        // Un passage par image ; pendant l'écran de chargement, le vaisseau vient d'arriver.
        if (ship == NullEntity || m_requestedPassage || m_loadingScreen->IsActive()) {
            return;
        }
        const Math::WorldPosition position = m_scene.GetWorldPosition(ship);
        for (const auto [entity, passage] : m_scene.GetRegistry().view<const PassageComponent>().each()) {
            if (m_scene.GetShip(entity) == ship) {
                continue; // porté par le vaisseau lui-même
            }
            const Math::Vector3f offset(Math::Delta(m_scene.GetWorldPosition(entity), position));
            const Math::Vector3f local = m_scene.GetWorldRotation(entity).Conjugate().Rotate(offset);
            if (std::abs(local.x) <= 0.5f * passage.size.x && std::abs(local.y) <= 0.5f * passage.size.y
                && std::abs(local.z) <= 0.5f * passage.size.z) {
                m_requestedPassage = PassageRequest{entity, ship};
                return;
            }
        }
    }

    void Engine::TakePassage() {
        const std::optional<PassageRequest> request = std::exchange(m_requestedPassage, std::nullopt);
        if (!request || m_gameState == GameState::Stopped || !m_scene.IsValid(request->passage) || !m_scene.IsValid(request->ship)
            || !m_scene.TryGet<PassageComponent>(request->passage)) {
            return;
        }
        const PassageComponent passage = m_scene.Get<PassageComponent>(request->passage);
        const Uuid passageId = m_scene.GetId(request->passage);
        const bool back = passage.scene.empty();
        if (back && !m_outsideScene) {
            Log::Warning("« {} » : aucune scène où revenir (le jeu a commencé ici)", NameOf(m_scene, request->passage));
            return;
        }
        if (!back && !m_project) {
            return;
        }

        // Le vaisseau, avec ce qu'il porte, écrit puis retiré de la scène quittée. Le siège du joueur n'est pas gardé
        // par la scène : repris par identifiant, comme pour la sauvegarde rapide.
        Entity ship = request->ship;
        const Entity player = GetFollowedPlayer();
        std::optional<Uuid> seat;
        if (const auto* character = player != NullEntity ? m_scene.TryGet<CharacterComponent>(player) : nullptr;
            character && character->seat != NullEntity && m_scene.IsValid(character->seat)) {
            seat = m_scene.GetId(character->seat);
        }
        PlaceRoot(m_scene, ship, m_scene.GetWorldPosition(ship), m_scene.GetWorldRotation(ship));
        const Uuid shipId = m_scene.GetId(ship);
        const std::string shipText = SceneSerializer::SaveEntity(m_scene, m_components, ship, true);
        std::vector<std::pair<std::string, std::string>> systems = m_quickSave->SaveSystems(*this);
        m_scene.DestroyEntity(ship);
        std::string outside = back ? std::string{} : SceneSerializer::SaveToString(m_scene, m_components, GetPrefabReader());

        const std::expected<void, std::string> loaded =
            back ? SceneSerializer::LoadFromString(m_scene, m_components, m_outsideScene->scene, GetPrefabReader())
                 : SceneSerializer::Load(m_scene, m_components, m_project->assetsDirectory / Platform::PathFromUtf8(passage.scene),
                                         GetPrefabReader());
        if (!loaded) {
            // Scène inchangée : le vaisseau y revient, là où il était.
            Log::Error("passage vers {} : {}", back ? std::string("la scène quittée") : passage.scene, loaded.error());
            Platform::ShowError(GetWindow(), passage.scene, loaded.error());
        }
        if (auto restored = SceneSerializer::RestoreEntity(m_scene, m_components, shipText); !restored) {
            Log::Error("passage : vaisseau perdu ({})", restored.error());
        }
        ship = m_scene.FindById(shipId);

        if (loaded && ship != NullEntity) {
            if (back) {
                // Devant le passage par lequel on était entré, le nez vers le dehors, lancé à sa vitesse de sortie.
                const Entity entry = m_scene.FindById(m_outsideScene->passage);
                if (entry != NullEntity) {
                    const auto* component = m_scene.TryGet<PassageComponent>(entry);
                    const Math::Quaternionf rotation = m_scene.GetWorldRotation(entry);
                    Math::WorldPosition position = m_scene.GetWorldPosition(entry);
                    position += Math::Vector3d(rotation.Rotate(Math::Vector3f{0.0f, 0.0f, component ? -component->exitDistance : 0.0f}));
                    PlaceRoot(m_scene, ship, position, rotation);
                }
                ShipComponent& component = m_scene.Get<ShipComponent>(ship);
                component.cruiseSpeed = std::max(-component.velocity.z, 0.0f);
            } else {
                // Sur l'entité d'arrivée ; posé si c'est un quai.
                const Entity arrival = passage.arrival.empty() ? NullEntity : m_scene.FindByName(passage.arrival);
                if (arrival != NullEntity) {
                    PlaceRoot(m_scene, ship, m_scene.GetWorldPosition(arrival), m_scene.GetWorldRotation(arrival));
                    if (m_scene.TryGet<DockComponent>(arrival)) {
                        Docking::Dock(m_scene, ship, arrival);
                    }
                } else {
                    Log::Warning("passage : arrivée « {} » introuvable dans {}", passage.arrival, passage.scene);
                }
            }
        }
        if (const Entity seatEntity = seat ? m_scene.FindById(*seat) : NullEntity; seatEntity != NullEntity) {
            for (auto [entity, character] : m_scene.GetRegistry().view<CharacterComponent>().each()) {
                if (character.player) {
                    character.seat = seatEntity;
                    character.seatState = SeatState::Seated;
                    character.seatTime = 0.0f;
                }
            }
        }

        // Nouvelle scène, ou scène retrouvée : physique refaite, systèmes du jeu prévenus.
        m_physics.reset();
        m_projectiles.clear();
        m_impacts.clear();
        ResetGameState();
        StartPhysics();
        m_loadingScreen->Begin();
        m_audio->StopAll();
        if (!loaded) {
            m_quickSave->LoadSystems(*this, systems); // la scène n'a pas changé : rien n'est oublié
        } else if (back) {
            m_quickSave->LoadSystems(*this, m_outsideScene->systems); // missions reprises telles qu'au départ
            m_outsideScene.reset();
        } else {
            m_outsideScene = OutsideScene{std::move(outside), passageId, std::move(systems)};
        }
    }
}
