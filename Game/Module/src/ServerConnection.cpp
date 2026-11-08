#include "ServerConnection.hpp"

#include "GameProtocol.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Core/Log.hpp>
#include <Flux/Net/Client.hpp>
#include <Flux/Platform/Console.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Game {
    namespace {
        using namespace Flux;

        constexpr const char* ServerConnectionSystem = "Jeu : connexion au serveur";

        std::optional<std::string> FindServerAddress() {
            const std::vector<std::string> arguments = Platform::GetCommandLineArguments();
            for (std::size_t i = 0; i + 1 < arguments.size(); ++i) {
                if (arguments[i] == "--serveur") {
                    return arguments[i + 1];
                }
            }
            return std::nullopt;
        }

        void ServeConnection(Net::Client& client) {
            for (const Net::Event& event : client.Poll()) {
                switch (event.kind) {
                    case Net::EventKind::Connected: Log::Info("Connecté au serveur"); break;
                    case Net::EventKind::Disconnected:
                        if (event.reason == Net::DisconnectReason::VersionMismatch) {
                            Log::Error("Serveur refusé : il n'a pas la même version du jeu que ce client");
                        } else {
                            Log::Warning("Déconnecté du serveur : {}", Net::ToString(event.reason));
                        }
                        break;
                    case Net::EventKind::Message: break; // aucun message du jeu avant l'étape 6
                }
            }
        }
    }

    void RegisterServerConnection(Engine& engine) {
        const std::optional<std::string> address = FindServerAddress();
        if (!address) {
            return;
        }
        // Possédé par le système : retiré au déchargement du module (rechargement à chaud), il se déconnecte.
        auto client = std::make_shared<Net::Client>();
        if (auto connecting = client->Connect(
                {.address = *address, .port = DefaultServerPort, .gameVersion = GameProtocolVersion});
            !connecting) {
            Log::Error("Serveur {} : {}", *address, connecting.error());
            return;
        }
        Log::Info("Connexion au serveur {}…", *address);
        // Un relevé par pas de simulation, comme le serveur. En pause, les événements attendent la reprise.
        engine.AddGameSystem(Engine::GameStage::Interaction, ServerConnectionSystem,
                             [client](Engine&, double) { ServeConnection(*client); });
    }

    void UnregisterServerConnection(Engine& engine) {
        engine.RemoveGameSystem(ServerConnectionSystem);
    }
}
