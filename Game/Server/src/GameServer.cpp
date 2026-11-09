#include "GameServer.hpp"

#include "GameProtocol.hpp"
#include "GameSim.hpp"
#include "SpawnPoint.hpp"

#include <Flux/Core/Log.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Scene/SceneSerializer.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <thread>
#include <utility>

namespace Game {
    namespace {
        using Clock = std::chrono::steady_clock;
        using Seconds = std::chrono::duration<double>;

        constexpr Seconds ReportInterval{10.0};

        void MarkSpawned(const Flux::Scene& scene, Flux::Entity entity, std::unordered_set<Flux::Uuid>& spawned) {
            spawned.insert(scene.GetId(entity));
            scene.ForEachChild(entity, [&](Flux::Entity child) { MarkSpawned(scene, child, spawned); });
        }
    }

    GameServer::GameServer() : m_state(RegisterSimulation(m_simulation)) {}

    GameServer::~GameServer() {
        m_simulation.Stop();
        UnregisterSimulation(m_simulation);
    }

    std::expected<void, std::string> GameServer::Open(Flux::Project project, const std::filesystem::path& scene) {
        const std::filesystem::path path = scene.empty() ? project.GetStartupScenePath() : project.assetsDirectory / scene;
        m_simulation.SetProject(std::move(project));
        if (auto loaded = m_simulation.LoadScene(path); !loaded) {
            return std::unexpected(std::format("scène {} : {}", Flux::Platform::ToUtf8(path), loaded.error()));
        }
        // Les personnages du solo partent : chaque joueur connecté aura le sien (point d'apparition).
        if (const int removed = SpawnPoints::RemovePlacedPlayers(m_simulation.GetScene()); removed > 0) {
            Flux::Log::Info("{} personnage(s) joueur(s) du solo retiré(s) : les joueurs apparaissent en se connectant", removed);
        }
        m_simulation.Start();
        m_scene = Flux::Platform::ToGenericUtf8(path.lexically_normal().lexically_relative(
            m_simulation.GetProject()->assetsDirectory.lexically_normal()));
        m_sceneEntities.clear();
        for (const auto [entity, id] : m_simulation.GetScene().GetRegistry().view<const Flux::IdComponent>().each()) {
            m_sceneEntities.insert(id.id);
        }
        m_schema.Build(m_simulation.GetComponents());
        m_history.Clear();
        Flux::Log::Info("Scène {} chargée, {} pas par seconde", Flux::Platform::ToUtf8(path),
                        static_cast<int>(1.0 / Flux::SimulationClock::StepSeconds + 0.5));
        return {};
    }

    std::expected<void, std::string> GameServer::Listen(std::uint16_t port) {
        if (auto listening = m_network.Listen({.port = port, .gameVersion = GameProtocolVersion}); !listening) {
            return listening;
        }
        Flux::Log::Info("En attente des clients sur le port UDP {} (protocole du jeu {}, FluxNet {})", port,
                        GameProtocolVersion, Flux::Net::ProtocolVersion);
        return {};
    }

    void GameServer::ServeNetwork() {
        for (const Flux::Net::Event& event : m_network.Poll()) {
            switch (event.kind) {
                case Flux::Net::EventKind::Connected:
                    Flux::Log::Info("Client {} connecté ({} en tout)", event.connection, m_network.GetConnectionCount());
                    m_clients[event.connection] = {};
                    SendWelcome(event.connection);
                    break;
                case Flux::Net::EventKind::Disconnected:
                    if (const auto client = m_clients.find(event.connection); client != m_clients.end()) {
                        Leave(client->second);
                        m_clients.erase(client);
                    }
                    Flux::Log::Info("Client {} déconnecté : {} ({} en tout)", event.connection,
                                    Flux::Net::ToString(event.reason), m_network.GetConnectionCount());
                    break;
                case Flux::Net::EventKind::Message: {
                    Flux::ByteReader reader(event.payload);
                    const auto type = static_cast<MessageType>(reader.Read<std::uint8_t>());
                    const auto client = m_clients.find(event.connection);
                    if (client == m_clients.end()) {
                        break;
                    }
                    switch (type) {
                        case MessageType::SnapshotAck: {
                            const auto sequence = reader.Read<std::uint32_t>();
                            // Le plus récent seulement : un accusé non fiable peut arriver après un plus récent.
                            if (reader.IsValid() && sequence > client->second.acknowledged) {
                                client->second.acknowledged = sequence;
                            }
                            break;
                        }
                        case MessageType::Join: Join(event.connection, client->second, reader); break;
                        case MessageType::Command: ReadCommands(client->second, reader); break;
                        default: break; // message du serveur ou inconnu : ignoré
                    }
                    break;
                }
            }
        }
    }

    void GameServer::SendWelcome(Flux::Net::ConnectionId connection) {
        m_message.Clear();
        m_message.Write(MessageType::Welcome);
        m_message.Write(m_schema.GetHash());
        m_message.WriteString(m_scene);
        m_message.Write(m_simulation.GetClock().GetStep());
        m_network.Send(connection, m_message.GetBytes(), Flux::Net::Delivery::Reliable);
    }

    void GameServer::Join(Flux::Net::ConnectionId connection, Client& client, Flux::ByteReader& reader) {
        std::string name = reader.ReadString();
        const std::string prefab = reader.ReadString();
        if (!reader.IsValid()) {
            Flux::Log::Warning("Client {} : demande pour rejoindre abîmée", connection);
            return;
        }
        if (client.player != Flux::NullEntity) {
            return; // déjà dans la zone
        }
        constexpr std::size_t MaxName = 32;
        if (name.empty() || name.size() > MaxName) {
            name = std::format("Joueur {}", connection);
        }
        auto spawned = SpawnPoints::Spawn(m_simulation, prefab, name);
        if (!spawned) {
            Flux::Log::Error("Client {} ({}) : pas de personnage, {}", connection, name, spawned.error());
            return;
        }
        client.name = std::move(name);
        client.player = *spawned;
        Flux::Log::Info("{} a rejoint la zone (client {})", client.name, connection);
        m_message.Clear();
        m_message.Write(MessageType::PlayerEntity);
        m_message.Write(m_simulation.GetScene().GetId(client.player).value);
        m_network.Send(connection, m_message.GetBytes(), Flux::Net::Delivery::Reliable);
    }

    void GameServer::ReadCommands(Client& client, Flux::ByteReader& reader) {
        const auto count = static_cast<std::size_t>(reader.Read<std::uint8_t>());
        if (!reader.IsValid() || count > MaxCommandsPerMessage) {
            return;
        }
        // De la plus récente à la plus ancienne : celles déjà reçues sont passées, les actions enfoncées des autres
        // s'ajoutent (un message perdu ne fait pas perdre un tir).
        for (std::size_t i = 0; i < count; ++i) {
            Flux::PlayerCommand command;
            if (!Flux::ReadCommand(reader, command)) {
                return;
            }
            if (command.step <= client.lastCommandStep && client.lastCommandStep != 0) {
                break;
            }
            client.pressed |= command.pressed;
            if (i == 0) {
                client.command = command;
            }
        }
        if (count > 0) {
            client.lastCommandStep = std::max(client.lastCommandStep, client.command.step);
        }
    }

    void GameServer::Leave(Client& client) {
        Flux::Scene& scene = m_simulation.GetScene();
        if (client.player != Flux::NullEntity && scene.IsValid(client.player)) {
            scene.DestroyEntity(client.player);
            Flux::Log::Info("{} a quitté la zone", client.name);
        }
        client.player = Flux::NullEntity;
    }

    void GameServer::ApplyCommands() {
        const Flux::Scene& scene = m_simulation.GetScene();
        for (auto& [connection, client] : m_clients) {
            if (client.player == Flux::NullEntity || !scene.IsValid(client.player)) {
                continue;
            }
            Flux::PlayerCommand command = client.command;
            command.pressed = std::exchange(client.pressed, {});
            m_simulation.SetPlayerCommand(client.player, command);
        }
    }

    void GameServer::Step() {
        ApplyCommands();
        m_simulation.Step(Flux::SimulationClock::StepSeconds);
    }

    void GameServer::SendSpawns(Flux::Net::ConnectionId connection, Client& client, const Flux::Replication::WorldState& state) {
        const Flux::Scene& scene = m_simulation.GetScene();
        for (const Flux::Replication::EntityState& entity : state.entities) {
            if (m_sceneEntities.contains(entity.id) || client.spawned.contains(entity.id)) {
                continue;
            }
            // La plus haute créée pendant le jeu, envoyée avec ses descendants : RestoreEntity la recrée sous son
            // parent de la scène (ou à la racine).
            Flux::Entity root = scene.FindById(entity.id);
            for (Flux::Entity parent = scene.GetParent(root);
                 parent != Flux::NullEntity && !m_sceneEntities.contains(scene.GetId(parent)); parent = scene.GetParent(parent)) {
                root = parent;
            }
            MarkSpawned(scene, root, client.spawned);
            m_message.Clear();
            m_message.Write(MessageType::EntitySpawn);
            m_message.Write(scene.GetId(root).value);
            m_message.Write(state.step);
            m_message.WriteString(Flux::SceneSerializer::SaveEntity(scene, m_simulation.GetComponents(), root, true));
            m_network.Send(connection, m_message.GetBytes(), Flux::Net::Delivery::Reliable);
        }
    }

    void GameServer::Replicate() {
        if (m_clients.empty()) {
            return; // personne à qui l'envoyer : pas d'instantané pris
        }
        Flux::Replication::WorldState& state = m_history.Next(m_simulation.GetClock().GetStep());
        m_capturer.Capture(m_simulation.GetScene(), m_schema, state);
        for (auto& [connection, client] : m_clients) {
            SendSpawns(connection, client, state);
            // Différences depuis le dernier instantané reçu, s'il est encore dans l'historique ; sinon en entier.
            const Flux::Replication::WorldState* baseline = m_history.Find(client.acknowledged);
            m_message.Clear();
            m_message.Write(MessageType::Snapshot);
            Flux::Replication::Encode(m_schema, state, baseline, m_message);
            m_network.Send(connection, m_message.GetBytes(), Flux::Net::Delivery::Unreliable);
        }
    }

    void GameServer::RunSteps(std::uint64_t count) {
        for (std::uint64_t i = 0; i < count; ++i) {
            Step();
        }
    }

    void GameServer::Run(const std::function<bool()>& shouldStop) {
        Flux::SimulationClock& clock = m_simulation.GetClock();
        Clock::time_point previous = Clock::now();
        Clock::time_point nextReport = previous + std::chrono::duration_cast<Clock::duration>(ReportInterval);
        // Bilan : temps passé à simuler, et tours où l'horloge a dû abandonner du temps (serveur trop lent).
        Seconds busy{0.0};
        std::uint64_t reportSteps = 0;
        int lateTurns = 0;
        while (!shouldStop()) {
            const Clock::time_point now = Clock::now();
            const int steps = clock.Advance(Seconds(now - previous).count());
            previous = now;
            ServeNetwork();
            for (int i = 0; i < steps; ++i) {
                Step();
            }
            if (clock.GetStep() >= m_lastSnapshotStep + SnapshotIntervalSteps) {
                Replicate();
                m_lastSnapshotStep = clock.GetStep();
            }
            m_network.Flush();
            reportSteps += static_cast<std::uint64_t>(steps);
            lateTurns += steps == Flux::SimulationClock::MaxStepsPerFrame ? 1 : 0;
            busy += Clock::now() - now;

            if (now >= nextReport) {
                const double stepMilliseconds = reportSteps > 0 ? busy.count() * 1000.0 / static_cast<double>(reportSteps) : 0.0;
                Flux::Log::Info("Pas {} : {:.2f} ms par pas, occupé {:.0f} % du temps, {} client(s)", clock.GetStep(),
                                stepMilliseconds, busy.count() * 100.0 / ReportInterval.count(),
                                m_network.GetConnectionCount());
                if (lateTurns > 0) {
                    Flux::Log::Warning("Serveur en retard : {} rattrapages de {} pas en 10 s", lateTurns,
                                       Flux::SimulationClock::MaxStepsPerFrame);
                }
                nextReport = now + std::chrono::duration_cast<Clock::duration>(ReportInterval);
                busy = Seconds{0.0};
                reportSteps = 0;
                lateTurns = 0;
            }

            // Jusqu'au pas suivant : l'horloge garde la part déjà écoulée. Une attente trop longue ne perd rien,
            // le pas en retard est rattrapé au tour suivant.
            std::this_thread::sleep_for(Seconds(Flux::SimulationClock::StepSeconds * (1.0 - clock.GetAlpha())));
        }
    }
}
