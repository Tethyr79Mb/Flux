#include "Flux/Core/Simulation.hpp"

#include "Flux/Animation/Animator.hpp"
#include "Flux/Physics/PhysicsContext.hpp"
#include "Flux/Physics/PhysicsWorld.hpp"
#include "Flux/Platform/Filesystem.hpp"
#include "Flux/Scene/Seats.hpp"
#include "Core/FlightSurroundings.hpp"

#include <utility>

namespace Flux {
    namespace {
        // Projectiles réservés d'avance : un combat chargé n'alloue pas en plein pas.
        constexpr std::size_t ReservedProjectiles = 4096;
    }

    Simulation::Simulation()
        : m_physicsContext(std::make_shared<PhysicsContext>()),
          m_flightSurroundings(std::make_unique<FlightSurroundings>()) {
        RegisterEngineComponents(m_components);
        m_projectiles.reserve(ReservedProjectiles);
        m_projectileHits.reserve(ReservedProjectiles);
    }

    // Défini ici, où PhysicsWorld et FlightSurroundings sont complets.
    Simulation::~Simulation() = default;

    void Simulation::SetProject(Project project) {
        m_prefabReader = SceneSerializer::ReadPrefabsFrom(project.assetsDirectory);
        m_project = std::move(project);
    }

    std::expected<void, std::string> Simulation::LoadScene(const std::filesystem::path& path) {
        if (auto loaded = SceneSerializer::Load(m_scene, m_components, path, m_prefabReader); !loaded) {
            return loaded;
        }
        SceneReplaced();
        return {};
    }

    void Simulation::SceneReplaced() {
        m_physics.reset(); // les corps de l'ancienne scène
        m_clock.Reset();
        m_projectiles.clear();
        m_projectileHits.clear();
        m_pilotInput = {};
        m_localCommand = {};
        RunSystems(Stage::Reset, 0.0);
        if (m_started) {
            StartPhysics();
        }
    }

    void Simulation::Start() {
        if (m_started) {
            return;
        }
        m_started = true;
        m_clock.Reset();
        StartPhysics();
    }

    void Simulation::Stop() {
        m_started = false;
        m_physics.reset();
        m_projectiles.clear();
        m_projectileHits.clear();
        m_pilotInput = {};
        m_localCommand = {};
        RunSystems(Stage::Reset, 0.0);
    }

    void Simulation::StartPhysics() {
        m_physicsContext->SetAssetsDirectory(m_project ? m_project->assetsDirectory : std::filesystem::path{});
        m_physics = std::make_unique<PhysicsWorld>(m_physicsContext);
        m_physics->Prepare(m_scene);
    }

    Entity Simulation::GetPlayer() const {
        if (!m_physics) {
            return NullEntity;
        }
        const Entity player = m_physics->GetPlayer();
        return m_scene.IsValid(player) ? player : NullEntity;
    }

    const PlayerCommand& Simulation::GetPlayerCommand(Entity player) const {
        static const PlayerCommand None;
        return player == GetPlayer() ? m_localCommand : None;
    }

    void Simulation::Step(double deltaSeconds) {
        if (!m_physics) {
            StartPhysics(); // scène changée sans LoadScene (par le code du jeu) : préparée maintenant
        }
        const Entity player = m_physics->GetPlayer();
        const auto* playerCharacter = m_scene.IsValid(player) ? m_scene.TryGet<CharacterComponent>(player) : nullptr;
        const bool seated = playerCharacter && playerCharacter->seat != NullEntity;
        const PlayerCommand& command = GetPlayerCommand(player);

        // Personnage du joueur, par rapport à sa vue ; assis, les mêmes actions pilotent le vaisseau (poste de
        // pilotage).
        CharacterInput character;
        // Systèmes du jeu d'abord : Interagir près d'un donneur de missions sert à lui parler, pas à s'asseoir.
        m_interactionPending = command.WasPressed(m_characterActions.interact);
        RunSystems(Stage::Interaction, deltaSeconds);
        const SeatInput seatInput{.interact = std::exchange(m_interactionPending, false)};
        // Commandes du jeu (étape Interaction, SetPilotInput), complétées ici du manche et de ce qui entoure le
        // vaisseau ; reprises à zéro à chaque pas.
        ShipInput shipInput = std::exchange(m_pilotInput, ShipInput{});
        if (seated) {
            shipInput.stickYaw = command.stickYaw;
            shipInput.stickPitch = command.stickPitch;
            // Aide au pilotage : selon la gravité du dehors (planète, sol), rien dans l'espace.
            if (const Entity ship = Ships::GetPilotedShip(m_scene, player); ship != NullEntity) {
                if (const Math::Vector3f gravity = m_physics->GetOutsideGravity(m_scene, ship); Math::Length(gravity) > 0.01f) {
                    shipInput.up = Math::Normalize(gravity) * -1.0f;
                }
                const FlightState flight = m_flightSurroundings->Get(m_scene, m_physics.get(), ship);
                shipInput.air = flight.air;
                if (flight.nearPlanet) {
                    shipInput.planetUp = Math::Vector3f(flight.up);
                    shipInput.planetDistance = static_cast<float>(flight.boundary);
                    shipInput.planetCenter = static_cast<float>(flight.center);
                }
            }
        } else {
            character.move = command.move;
            character.run = command.IsDown(m_characterActions.run);
            character.jump = command.WasPressed(m_characterActions.jump);
        }
        Seats::Update(m_scene, player, deltaSeconds, seatInput, [this](Entity entity) {
            // Durées des animations « s'asseoir » et « se lever » de son modèle, à la vitesse de lecture.
            const auto* animator = m_scene.TryGet<AnimatorComponent>(entity);
            const auto* model = m_scene.TryGet<ModelComponent>(entity);
            if (!m_animationDurations || !m_project || !animator || !model || model->model.empty() || animator->speed <= 0.0f) {
                return Seats::Durations{};
            }
            const std::filesystem::path path = m_project->assetsDirectory / Platform::PathFromUtf8(model->model);
            const auto duration = [&](const std::string& name) {
                return name.empty() ? 0.0f : m_animationDurations(path, name) / animator->speed;
            };
            return Seats::Durations{duration(animator->sitDownAnimation), duration(animator->standUpAnimation)};
        });
        Ships::Update(m_scene, Ships::GetPilotedShip(m_scene, player), deltaSeconds, shipInput);
        RunSystems(Stage::BeforePhysics, deltaSeconds); // pilote automatique, warp et IA du jeu, après le pilote
        m_physics->Update(m_scene, deltaSeconds, character);
        // Projectiles après la physique, avant les dégâts et les armes du jeu (étape AfterPhysics) : les vaisseaux
        // sont à leur place de ce pas, les nouveaux tirs partent de la bouche des canons (avant, ils naîtraient en
        // retard d'un pas : 16 m à 1 000 m/s). Les anciens avancent d'abord.
        m_projectileHits.clear();
        Projectiles::GuideMissiles(m_scene, m_projectiles, deltaSeconds);
        Projectiles::Update(
            m_projectiles, deltaSeconds,
            [this](const Math::WorldPosition& from, const Math::Vector3f& segment, Entity ship) -> std::optional<std::pair<Entity, float>> {
                if (const auto hit = m_physics->CastRay(m_scene, from, segment, ship)) {
                    return std::pair{hit->entity, hit->fraction};
                }
                return std::nullopt;
            },
            &m_projectileHits);
        RunSystems(Stage::AfterPhysics, deltaSeconds); // dégâts (GetProjectileHits), armes du joueur et des ennemis
        Animator::Update(m_scene, deltaSeconds); // après la physique : les pas du personnage de ce pas
    }

    bool Simulation::ConsumeInteraction() {
        return std::exchange(m_interactionPending, false);
    }

    FlightState Simulation::GetFlightState(Entity ship) {
        return ship != NullEntity && m_scene.IsValid(ship) ? m_flightSurroundings->Get(m_scene, m_physics.get(), ship) : FlightState{};
    }

    void Simulation::AddSystem(Stage stage, std::string name, StepFunction function) {
        RemoveSystem(name);
        m_systems.push_back({stage, std::move(name), std::move(function)});
    }

    void Simulation::RemoveSystem(std::string_view name) {
        std::erase_if(m_systems, [&](const System& system) { return system.name == name; });
    }

    void Simulation::ClearSystems() {
        m_systems.clear();
    }

    void Simulation::RunSystems(Stage stage, double deltaSeconds) {
        // Par indice et par copie : un système peut en ajouter ou en retirer un autre.
        for (std::size_t i = 0; i < m_systems.size(); ++i) {
            if (m_systems[i].stage == stage && m_systems[i].function) {
                const StepFunction function = m_systems[i].function;
                function(*this, deltaSeconds);
            }
        }
    }
}
