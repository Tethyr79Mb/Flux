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

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Game {
    namespace {
        using namespace Flux;

        constexpr const char* ServerConnectionSystem = "Jeu : connexion au serveur";
        constexpr const char* ServerSceneSystem = "Jeu : scène du serveur";
        // Noms vus dans les interfaces (boutons « Fonction C++ », liaisons des textes et des champs de texte).
        constexpr const char* JoinFunction = "Rejoindre";
        constexpr const char* LeaveFunction = "Quitter le serveur";
        constexpr const char* PlayerNameValue = "Nom du joueur";
        constexpr const char* ServerAddressValue = "Adresse du serveur";
        constexpr const char* LostReasonValue = "Raison de la coupure";
        // Fenêtre (ConnexionPerdue.fluxui, posée dans les scènes de jeu) ouverte quand la connexion se coupe.
        constexpr const char* LostWindow = "Connexion perdue";
        constexpr const char* DefaultServerAddress = "127.0.0.1";

        // Valeur de l'option `name` de la ligne de commande (« --serveur 127.0.0.1 »).
        std::optional<std::string> FindArgument(std::string_view name) {
            const std::vector<std::string> arguments = Platform::GetCommandLineArguments();
            for (std::size_t i = 0; i + 1 < arguments.size(); ++i) {
                if (arguments[i] == name) {
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
            ByteWriter message; // accusés de réception et commandes, gardé d'un envoi à l'autre
            std::string playerName; // envoyé pour rejoindre la zone (Join)
            bool lost = false;      // coupée : le monde reste figé derrière la boîte de dialogue
            std::string scene;  // relative au dossier des assets, donnée par le serveur (Welcome)
            Uuid player;        // personnage de ce joueur, donné par le serveur (PlayerEntity) ; vide : pas encore
            // Dernières commandes de cet écran, la plus récente d'abord : répétées dans chaque envoi (message perdu).
            std::array<PlayerCommand, MaxCommandsPerMessage> commands{};
            std::size_t commandCount = 0;
            bool sceneReady = false; // la scène chargée est celle du serveur : les instantanés s'y appliquent
            // Prochain état appliqué : regarder toute la scène (entités détruites par le serveur avant notre arrivée,
            // ou scène rechargée), pas seulement ce qui a disparu depuis le précédent.
            bool fullSync = true;
            std::uint32_t lastApplied = 0; // numéro de l'instantané « to » appliqué en dernier
            // Entités créées pendant le jeu (EntitySpawn), dans l'ordre d'arrivée : remises après un rechargement de la
            // scène (un enfant après son parent).
            struct Spawn
            {
                Uuid id;
                std::uint64_t step = 0; // premier instantané qui la contient
                std::string text;
            };
            std::vector<Spawn> spawns;
            std::vector<Uuid> pendingSpawns; // créées après l'instantané affiché : DestroyMissing les garde
        };

        bool IsServerScene(Engine& engine, const ServerConnection& connection) {
            const Project* project = engine.GetSimulation().GetProject();
            if (project == nullptr || connection.scene.empty()) {
                return false;
            }
            const std::filesystem::path expected = project->assetsDirectory / Platform::PathFromUtf8(connection.scene);
            return expected.lexically_normal() == engine.GetScenePath().lexically_normal();
        }

        // Ce qui reste d'une connexion à l'autre : nom et adresse (gardés dans Joueur.json), scène où revenir.
        struct ServerLink
        {
            std::string playerName;
            std::string address;     // adresse[:port] ; vide : DefaultServerAddress
            std::string returnScene; // relative au dossier des assets : celle d'où l'on a rejoint
            std::string lostReason;  // dite par la fenêtre « Connexion perdue »
            std::shared_ptr<ServerConnection> connection; // nullptr : pas connecté
        };

        // À côté de l'exe, comme les réglages : pas de comptes pendant le développement (Bruno, 2026-11-09).
        std::filesystem::path LinkFile() {
            return Platform::GetExecutableDirectory() / "Joueur.json";
        }

        void LoadLink(ServerLink& link) {
            std::ifstream file(LinkFile());
            if (!file) {
                return; // premier lancement
            }
            try {
                const nlohmann::json json = nlohmann::json::parse(file);
                link.playerName = json.value("nom", std::string{});
                link.address = json.value("serveur", std::string{});
            } catch (const nlohmann::json::exception& error) {
                Log::Warning("Joueur.json : {}", error.what());
            }
        }

        void SaveLink(const ServerLink& link) {
            std::ofstream file(LinkFile());
            file << nlohmann::json{{"nom", link.playerName}, {"serveur", link.address}}.dump(2) << '\n';
            if (!file) {
                Log::Warning("Joueur.json : impossible d'écrire {}", Platform::ToGenericUtf8(LinkFile()));
            }
        }

        // Fin de la connexion (bouton, ou OK de la fenêtre « Connexion perdue ») : retour à la scène d'où l'on a
        // rejoint, simulée de nouveau ici.
        void Leave(Engine& engine, ServerLink& link) {
            if (!link.connection) {
                return;
            }
            engine.RemoveGameSystem(ServerConnectionSystem);
            engine.RemoveGameSystem(ServerSceneSystem);
            link.connection.reset(); // se déconnecte
            engine.GetSimulation().SetLocalPlayer(NullEntity);
            engine.GetSimulation().SetReplicated(false);
            if (!link.returnScene.empty()) {
                engine.RequestScene(link.returnScene);
            }
        }

        // Coupée par le serveur ou le réseau : le monde reste figé derrière la fenêtre « Connexion perdue » (son OK
        // appelle « Quitter le serveur ») ; une scène qui ne l'a pas revient tout de suite à la sélection.
        void ConnectionLost(Engine& engine, ServerLink& link, Net::DisconnectReason reason) {
            if (reason == Net::DisconnectReason::VersionMismatch) {
                link.lostReason = "Le serveur n'a pas la même version du jeu.";
            } else if (reason == Net::DisconnectReason::ServerShutdown) {
                link.lostReason = "Le serveur s'est arrêté.";
            } else if (reason == Net::DisconnectReason::Lost && !link.connection->sceneReady) {
                link.lostReason = "Serveur injoignable.";
            } else {
                link.lostReason = std::string("Connexion ") + Net::ToString(reason) + ".";
            }
            Log::Warning("Déconnecté du serveur : {}", link.lostReason);
            link.connection->lost = true;
            if (!engine.SetInterfaceWindowOpen(LostWindow, true)) {
                Leave(engine, link);
            }
        }

        void RestoreSpawn(Engine& engine, const std::string& text) {
            if (auto restored = SceneSerializer::RestoreEntity(engine.GetScene(), engine.GetComponents(), text); !restored) {
                Log::Warning("Entité envoyée par le serveur : {}", restored.error());
            }
        }

        // false : serveur refusé (accueil abîmé, ou il ne réplique pas les mêmes champs).
        bool ReadWelcome(Engine& engine, ServerConnection& connection, ByteReader& reader) {
            const auto hash = reader.Read<std::uint64_t>();
            std::string scene = reader.ReadString();
            reader.Read<std::uint64_t>(); // pas du serveur : servira à la prédiction (étape 7)
            if (!reader.IsValid()) {
                Log::Error("Serveur : message d'accueil abîmé");
                return false;
            }
            if (hash != connection.schema.GetHash()) {
                // Même protocole annoncé, champs répliqués différents : GameProtocolVersion oublié.
                Log::Error("Serveur refusé : il ne réplique pas les mêmes champs que ce client (GameProtocolVersion à "
                           "incrémenter)");
                return false;
            }
            connection.scene = std::move(scene);
            connection.receiver.Clear();
            connection.lastApplied = 0;
            connection.fullSync = true;
            connection.spawns.clear();
            connection.player = {};
            connection.commandCount = 0;
            // Rejoindre la zone : un personnage au point d'apparition (celui du point : pas encore de choix).
            connection.message.Clear();
            connection.message.Write(MessageType::Join);
            connection.message.WriteString(connection.playerName);
            connection.message.WriteString("");
            connection.client.Send(connection.message.GetBytes(), Net::Delivery::Reliable);
            connection.sceneReady = IsServerScene(engine, connection);
            if (!connection.sceneReady) {
                Log::Info("Scène du serveur : {}", connection.scene);
                engine.RequestScene(connection.scene); // chargée à la fin de l'image, puis l'étape Reset
            }
            return true;
        }

        // La commande de cet écran pour ce pas, avec les précédentes. Le personnage est retrouvé à chaque pas : il
        // arrive après PlayerEntity (EntitySpawn), et change d'entité quand la scène est rechargée.
        void SendCommand(Engine& engine, ServerConnection& connection) {
            Simulation& simulation = engine.GetSimulation();
            const Entity player = connection.sceneReady && connection.player != Uuid{}
                                      ? engine.GetScene().FindById(connection.player)
                                      : NullEntity;
            simulation.SetLocalPlayer(player);
            if (player == NullEntity || !connection.client.IsConnected()) {
                return;
            }
            std::shift_right(connection.commands.begin(), connection.commands.end(), 1);
            connection.commands.front() = simulation.GetPlayerCommand(player);
            connection.commandCount = std::min(connection.commandCount + 1, connection.commands.size());
            connection.message.Clear();
            connection.message.Write(MessageType::Command);
            connection.message.Write(static_cast<std::uint8_t>(connection.commandCount));
            for (std::size_t i = 0; i < connection.commandCount; ++i) {
                WriteCommand(connection.message, connection.commands[i]);
            }
            connection.client.Send(connection.message.GetBytes(), Net::Delivery::Unreliable);
        }

        void ServeConnection(Engine& engine, ServerLink& link) {
            // Gardée jusqu'à la fin même si la connexion se termine en route (Leave) : les événements y vivent.
            const std::shared_ptr<ServerConnection> kept = link.connection;
            if (!kept || kept->lost) {
                return;
            }
            ServerConnection& connection = *kept;
            for (const Net::Event& event : connection.client.Poll()) {
                switch (event.kind) {
                    case Net::EventKind::Connected: Log::Info("Connecté au serveur"); break;
                    case Net::EventKind::Disconnected:
                        ConnectionLost(engine, link, event.reason);
                        return;
                    case Net::EventKind::Message: {
                        ByteReader reader(event.payload);
                        switch (static_cast<MessageType>(reader.Read<std::uint8_t>())) {
                            case MessageType::Welcome:
                                if (!ReadWelcome(engine, connection, reader)) {
                                    connection.client.Disconnect();
                                    ConnectionLost(engine, link, Net::DisconnectReason::VersionMismatch);
                                    return;
                                }
                                break;
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
                            case MessageType::PlayerEntity: {
                                const Uuid id{reader.Read<std::uint64_t>()};
                                if (reader.IsValid()) {
                                    connection.player = id;
                                    Log::Info("Personnage reçu du serveur");
                                }
                                break;
                            }
                            case MessageType::EntitySpawn: {
                                const Uuid id{reader.Read<std::uint64_t>()};
                                const auto step = reader.Read<std::uint64_t>();
                                std::string text = reader.ReadString();
                                if (!reader.IsValid()) {
                                    Log::Warning("Entité envoyée par le serveur : message abîmé");
                                    break;
                                }
                                if (connection.sceneReady) {
                                    RestoreSpawn(engine, text);
                                }
                                connection.spawns.push_back({.id = id, .step = step, .text = std::move(text)});
                                break;
                            }
                            default: break; // message du client (SnapshotAck, Join, Command) ou inconnu : ignoré
                        }
                        break;
                    }
                }
            }
            SendCommand(engine, connection);
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
                // Entités créées pendant le jeu que to devait avoir et n'a plus : détruites par le serveur, plus à remettre.
                std::erase_if(connection.spawns, [&](const auto& spawn) { return spawn.step <= to->step && to->Find(spawn.id) < 0; });
                connection.pendingSpawns.clear();
                for (const auto& spawn : connection.spawns) {
                    if (spawn.step > to->step) {
                        connection.pendingSpawns.push_back(spawn.id);
                    }
                }
                Replication::DestroyMissing(scene, previous, *to, connection.pendingSpawns);
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
            // Entités créées pendant le jeu : remises, sauf celles qui ne sont plus dans le dernier instantané (qui devait
            // déjà les avoir).
            const Replication::WorldState* latest = connection.receiver.GetLatest();
            std::erase_if(connection.spawns,
                          [&](const auto& spawn) { return latest && spawn.step <= latest->step && latest->Find(spawn.id) < 0; });
            for (const auto& spawn : connection.spawns) {
                RestoreSpawn(engine, spawn.text);
            }
        }
    }

    void RegisterServerConnection(Engine& engine) {
        // Possédé par les fonctions, les liaisons et les systèmes : retirés au déchargement du module (rechargement à
        // chaud), la connexion se ferme avec eux.
        auto link = std::make_shared<ServerLink>();
        LoadLink(*link);
        // La ligne de commande l'emporte sur Joueur.json : deux clients sur le même PC (--joueur Autre).
        link->playerName = FindArgument("--joueur").value_or(link->playerName);
        link->address = FindArgument("--serveur").value_or(link->address.empty() ? DefaultServerAddress : link->address);
        engine.BindInterfaceText(
            PlayerNameValue, [link] { return link->playerName; }, [link](std::string name) { link->playerName = std::move(name); });
        engine.BindInterfaceText(
            ServerAddressValue, [link] { return link->address; }, [link](std::string address) { link->address = std::move(address); });
        engine.BindInterfaceText(LostReasonValue, [link] { return link->lostReason; });

        engine.RegisterInterfaceFunction(JoinFunction, [link](Engine& engine) {
            if (link->connection) {
                return; // déjà connecté, ou en route
            }
            SaveLink(*link);
            const Project* project = engine.GetProject();
            if (project) {
                const std::filesystem::path scene = engine.GetScenePath().empty()
                                                        ? project->startupScene
                                                        : engine.GetScenePath().lexically_relative(project->assetsDirectory);
                link->returnScene = Platform::ToGenericUtf8(scene);
            }
            auto connection = std::make_shared<ServerConnection>();
            // Sans nom, le serveur en donne un.
            connection->playerName = link->playerName;
            // Après l'enregistrement des composants du jeu (RegisterSimulation) : ses champs répliqués en font partie.
            connection->schema.Build(engine.GetComponents());
            const std::string address = link->address.empty() ? DefaultServerAddress : link->address;
            if (auto connecting = connection->client.Connect(
                    {.address = address, .port = DefaultServerPort, .gameVersion = GameProtocolVersion});
                !connecting) {
                Log::Error("Serveur {} : {}", address, connecting.error());
                return;
            }
            Log::Info("Connexion au serveur {}…", address);
            link->connection = std::move(connection);
            link->lostReason.clear();
            // Le serveur simule, ce client affiche ce qu'il reçoit.
            engine.GetSimulation().SetReplicated(true);
            // Un relevé par pas de simulation, comme le serveur, à la seule étape qui tourne encore. En pause, les
            // messages attendent la reprise.
            engine.AddGameSystem(Engine::GameStage::Replication, ServerConnectionSystem,
                                 [link](Engine& engine, double) { ServeConnection(engine, *link); });
            engine.AddGameSystem(Engine::GameStage::Reset, ServerSceneSystem, [link](Engine& engine, double) {
                if (link->connection) {
                    SceneReset(engine, *link->connection);
                }
            });
        });
        engine.RegisterInterfaceFunction(LeaveFunction, [link](Engine& engine) { Leave(engine, *link); });
    }

    void UnregisterServerConnection(Engine& engine) {
        engine.RemoveGameSystem(ServerConnectionSystem);
        engine.RemoveGameSystem(ServerSceneSystem);
        engine.UnregisterInterfaceFunction(JoinFunction);
        engine.UnregisterInterfaceFunction(LeaveFunction);
        for (const char* value : {PlayerNameValue, ServerAddressValue, LostReasonValue}) {
            engine.UnbindInterfaceValue(value);
        }
        engine.GetSimulation().SetLocalPlayer(NullEntity);
        engine.GetSimulation().SetReplicated(false);
    }
}
