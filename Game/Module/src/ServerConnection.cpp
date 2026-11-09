#include "ServerConnection.hpp"

#include "GameProtocol.hpp"

#include <Flux/Core/ByteStream.hpp>
#include <Flux/Core/Engine.hpp>
#include <Flux/Core/Log.hpp>
#include <Flux/Net/Client.hpp>
#include <Flux/Platform/Console.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Scene/Replication.hpp>
#include <Flux/Scene/SceneSerializer.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Game {
    namespace {
        using namespace Flux;

        constexpr const char* ServerConnectionSystem = "Jeu : connexion au serveur";
        constexpr const char* ServerSceneSystem = "Jeu : scène du serveur";

        std::optional<std::string> FindServerAddress() {
            const std::vector<std::string> arguments = Platform::GetCommandLineArguments();
            for (std::size_t i = 0; i + 1 < arguments.size(); ++i) {
                if (arguments[i] == "--serveur") {
                    return arguments[i + 1];
                }
            }
            return std::nullopt;
        }

        // Le monde vu depuis le serveur : instantanés reçus, entités créées pendant le jeu, scène attendue.
        struct ServerConnection
        {
            Net::Client client;
            Replication::Schema schema;
            Replication::SnapshotReceiver receiver;
            ByteWriter message; // accusés de réception, gardé d'un envoi à l'autre
            std::string scene;  // relative au dossier des assets, donnée par le serveur (Welcome)
            bool sceneReady = false; // la scène chargée est celle du serveur : les instantanés s'y appliquent
            // Prochain état appliqué : regarder toute la scène (entités détruites par le serveur avant notre arrivée,
            // ou scène rechargée), pas seulement ce qui a disparu depuis le précédent.
            bool fullSync = true;
            std::uint32_t lastApplied = 0; // numéro de l'instantané « to » appliqué en dernier
            // Entités créées pendant le jeu (EntitySpawn), dans l'ordre d'arrivée : remises après un rechargement de la
            // scène (un enfant après son parent).
            std::vector<std::pair<Uuid, std::string>> spawns;
        };

        bool IsServerScene(Engine& engine, const ServerConnection& connection) {
            const Project* project = engine.GetSimulation().GetProject();
            if (project == nullptr || connection.scene.empty()) {
                return false;
            }
            const std::filesystem::path expected = project->assetsDirectory / Platform::PathFromUtf8(connection.scene);
            return expected.lexically_normal() == engine.GetScenePath().lexically_normal();
        }

        void RestoreSpawn(Engine& engine, const std::string& text) {
            if (auto restored = SceneSerializer::RestoreEntity(engine.GetScene(), engine.GetComponents(), text); !restored) {
                Log::Warning("Entité envoyée par le serveur : {}", restored.error());
            }
        }

        void ReadWelcome(Engine& engine, ServerConnection& connection, ByteReader& reader) {
            const auto hash = reader.Read<std::uint64_t>();
            std::string scene = reader.ReadString();
            reader.Read<std::uint64_t>(); // pas du serveur : servira à la prédiction (étape 7)
            if (!reader.IsValid()) {
                Log::Error("Serveur : message d'accueil abîmé");
                connection.client.Disconnect();
                return;
            }
            if (hash != connection.schema.GetHash()) {
                // Même protocole annoncé, champs répliqués différents : GameProtocolVersion oublié.
                Log::Error("Serveur refusé : il ne réplique pas les mêmes champs que ce client (GameProtocolVersion à "
                           "incrémenter)");
                connection.client.Disconnect();
                return;
            }
            connection.scene = std::move(scene);
            connection.receiver.Clear();
            connection.lastApplied = 0;
            connection.fullSync = true;
            connection.spawns.clear();
            connection.sceneReady = IsServerScene(engine, connection);
            if (!connection.sceneReady) {
                Log::Info("Scène du serveur : {}", connection.scene);
                engine.RequestScene(connection.scene); // chargée à la fin de l'image, puis l'étape Reset
            }
        }

        void ServeConnection(Engine& engine, ServerConnection& connection) {
            for (const Net::Event& event : connection.client.Poll()) {
                switch (event.kind) {
                    case Net::EventKind::Connected: Log::Info("Connecté au serveur"); break;
                    case Net::EventKind::Disconnected:
                        // Le monde reste figé à son dernier état (boîte de dialogue et retour au menu : étape 6c).
                        if (event.reason == Net::DisconnectReason::VersionMismatch) {
                            Log::Error("Serveur refusé : il n'a pas la même version du jeu que ce client");
                        } else {
                            Log::Warning("Déconnecté du serveur : {}", Net::ToString(event.reason));
                        }
                        break;
                    case Net::EventKind::Message: {
                        ByteReader reader(event.payload);
                        switch (static_cast<MessageType>(reader.Read<std::uint8_t>())) {
                            case MessageType::Welcome: ReadWelcome(engine, connection, reader); break;
                            case MessageType::Snapshot: {
                                auto received = connection.receiver.Receive(connection.schema, reader);
                                if (!received) {
                                    Log::Warning("Instantané du serveur : {}", received.error());
                                } else if (*received != 0) {
                                    connection.message.Clear();
                                    connection.message.Write(MessageType::SnapshotAck);
                                    connection.message.Write(*received);
                                    connection.client.Send(connection.message.GetBytes(), Net::Delivery::Unreliable);
                                }
                                break;
                            }
                            case MessageType::EntitySpawn: {
                                const Uuid id{reader.Read<std::uint64_t>()};
                                std::string text = reader.ReadString();
                                if (!reader.IsValid()) {
                                    Log::Warning("Entité envoyée par le serveur : message abîmé");
                                    break;
                                }
                                if (connection.sceneReady) {
                                    RestoreSpawn(engine, text);
                                }
                                connection.spawns.emplace_back(id, std::move(text));
                                break;
                            }
                            default: break; // message du client (SnapshotAck) ou inconnu : ignoré
                        }
                        break;
                    }
                }
            }
            connection.client.Flush();

            if (!connection.sceneReady) {
                return;
            }
            // Le monde affiché un peu en retard sur le dernier instantané, entre les deux qui l'encadrent.
            connection.receiver.Advance();
            const Replication::WorldState* from = nullptr;
            const Replication::WorldState* to = nullptr;
            float t = 0.0f;
            if (!connection.receiver.Sample(from, to, t)) {
                return;
            }
            Scene& scene = engine.GetScene();
            if (connection.fullSync || to->sequence != connection.lastApplied) {
                // Sans précédent (premier état, scène rechargée, précédent déjà remplacé dans l'anneau) : toute la scène.
                const Replication::WorldState* previous =
                    connection.fullSync ? nullptr : connection.receiver.Find(connection.lastApplied);
                Replication::DestroyMissing(scene, previous, *to);
                connection.fullSync = false;
                connection.lastApplied = to->sequence;
            }
            Replication::Apply(scene, connection.schema, *from, *to, t);
        }

        // Scène rechargée (celle du serveur, ou une autre) : ses entités sont celles du fichier.
        void SceneReset(Engine& engine, ServerConnection& connection) {
            connection.sceneReady = IsServerScene(engine, connection);
            connection.fullSync = true;
            if (!connection.sceneReady) {
                return;
            }
            // Entités créées pendant le jeu : remises, sauf celles qui ne sont plus dans le dernier instantané.
            const Replication::WorldState* latest = connection.receiver.GetLatest();
            std::erase_if(connection.spawns, [&](const auto& spawn) { return latest && latest->Find(spawn.first) < 0; });
            for (const auto& [id, text] : connection.spawns) {
                RestoreSpawn(engine, text);
            }
        }
    }

    void RegisterServerConnection(Engine& engine) {
        const std::optional<std::string> address = FindServerAddress();
        if (!address) {
            return;
        }
        // Possédé par les systèmes : retiré au déchargement du module (rechargement à chaud), il se déconnecte.
        auto connection = std::make_shared<ServerConnection>();
        // Après l'enregistrement des composants du jeu (RegisterSimulation) : ses champs répliqués en font partie.
        connection->schema.Build(engine.GetComponents());
        if (auto connecting = connection->client.Connect(
                {.address = *address, .port = DefaultServerPort, .gameVersion = GameProtocolVersion});
            !connecting) {
            Log::Error("Serveur {} : {}", *address, connecting.error());
            return;
        }
        Log::Info("Connexion au serveur {}…", *address);
        // Le serveur simule, ce client affiche ce qu'il reçoit.
        engine.GetSimulation().SetReplicated(true);
        // Un relevé par pas de simulation, comme le serveur, à la seule étape qui tourne encore. En pause, les messages
        // attendent la reprise.
        engine.AddGameSystem(Engine::GameStage::Replication, ServerConnectionSystem,
                             [connection](Engine& engine, double) { ServeConnection(engine, *connection); });
        engine.AddGameSystem(Engine::GameStage::Reset, ServerSceneSystem,
                             [connection](Engine& engine, double) { SceneReset(engine, *connection); });
    }

    void UnregisterServerConnection(Engine& engine) {
        engine.RemoveGameSystem(ServerConnectionSystem);
        engine.RemoveGameSystem(ServerSceneSystem);
        engine.GetSimulation().SetReplicated(false);
    }
}
