// Réseau (FluxNet) : un serveur et un client dans le même processus, sur 127.0.0.1 (vraies prises UDP, chiffrées).
// Poignée de main, messages dans les deux sens, refus d'un client d'une autre version, déconnexions.
#include "Check.hpp"

#include "Flux/Net/Client.hpp"
#include "Flux/Net/Server.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace Flux;

namespace {
    struct Received {
        Net::EventKind kind;
        Net::ConnectionId connection;
        Net::DisconnectReason reason;
        std::vector<std::byte> payload;
    };

    void Collect(std::span<const Net::Event> events, std::vector<Received>& out) {
        for (const Net::Event& event : events) {
            out.push_back({event.kind, event.connection, event.reason, {event.payload.begin(), event.payload.end()}});
        }
    }

    // Fait tourner serveur et client jusqu'à ce que done rende vrai (5 s au plus : la boucle locale répond en
    // quelques millisecondes, la marge couvre une machine de CI chargée).
    bool Pump(Net::Server* server, Net::Client* client, std::vector<Received>& serverEvents,
              std::vector<Received>& clientEvents, const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!done()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            if (server != nullptr) {
                Collect(server->Poll(), serverEvents);
            }
            if (client != nullptr) {
                Collect(client->Poll(), clientEvents);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    // Un port libre parmi quelques-uns : une autre instance des tests (ou un serveur lancé à côté) peut en tenir un.
    bool ListenOnFreePort(Net::Server& server, std::uint32_t gameVersion) {
        for (std::uint16_t port = 41'000; port < 41'050; ++port) {
            if (server.Listen({.port = port, .gameVersion = gameVersion})) {
                return true;
            }
        }
        return false;
    }

    bool Has(const std::vector<Received>& events, Net::EventKind kind) {
        for (const Received& event : events) {
            if (event.kind == kind) {
                return true;
            }
        }
        return false;
    }

    bool ConnectExchangeDisconnect() {
        Net::Server server;
        CHECK(ListenOnFreePort(server, 7));
        CHECK(server.GetPort() != 0);
        Net::Client client;
        CHECK(client.Connect({.address = "127.0.0.1", .port = server.GetPort(), .gameVersion = 7}));

        std::vector<Received> serverEvents;
        std::vector<Received> clientEvents;
        CHECK(Pump(&server, &client, serverEvents, clientEvents, [&] { return client.IsConnected(); }));
        CHECK(Pump(&server, &client, serverEvents, clientEvents, [&] { return server.GetConnectionCount() == 1; }));
        CHECK(Has(serverEvents, Net::EventKind::Connected));
        CHECK(Has(clientEvents, Net::EventKind::Connected));
        const Net::ConnectionId connection = serverEvents.front().connection;

        // Le type 0 est à FluxNet : refusé dans les deux sens, comme un message vide.
        const std::array reserved{std::byte{0}, std::byte{1}};
        CHECK(!client.Send(reserved, Net::Delivery::Reliable));
        CHECK(!server.Send(connection, reserved, Net::Delivery::Reliable));
        CHECK(!client.Send({}, Net::Delivery::Reliable));

        const std::array hello{std::byte{1}, std::byte{'a'}, std::byte{'b'}};
        CHECK(client.Send(hello, Net::Delivery::Reliable));
        client.Flush();
        serverEvents.clear();
        CHECK(Pump(&server, &client, serverEvents, clientEvents,
                   [&] { return Has(serverEvents, Net::EventKind::Message); }));
        CHECK(serverEvents.back().connection == connection);
        CHECK(serverEvents.back().payload == std::vector<std::byte>(hello.begin(), hello.end()));

        const std::array state{std::byte{2}, std::byte{42}};
        CHECK(server.Send(connection, state, Net::Delivery::Unreliable));
        server.Flush();
        clientEvents.clear();
        CHECK(Pump(&server, &client, serverEvents, clientEvents,
                   [&] { return Has(clientEvents, Net::EventKind::Message); }));
        CHECK(clientEvents.back().payload == std::vector<std::byte>(state.begin(), state.end()));

        client.Disconnect();
        CHECK(!client.IsConnected());
        serverEvents.clear();
        CHECK(Pump(&server, &client, serverEvents, clientEvents,
                   [&] { return Has(serverEvents, Net::EventKind::Disconnected); }));
        CHECK(serverEvents.back().reason == Net::DisconnectReason::Closed);
        CHECK(server.GetConnectionCount() == 0);
        return true;
    }

    bool OtherVersionRefused() {
        Net::Server server;
        CHECK(ListenOnFreePort(server, 7));
        Net::Client client;
        // Port dans l'adresse : il l'emporte sur celui de la configuration.
        const std::string address = "127.0.0.1:" + std::to_string(server.GetPort());
        CHECK(client.Connect({.address = address, .port = 1, .gameVersion = 8}));

        std::vector<Received> serverEvents;
        std::vector<Received> clientEvents;
        CHECK(Pump(&server, &client, serverEvents, clientEvents,
                   [&] { return Has(clientEvents, Net::EventKind::Disconnected); }));
        CHECK(clientEvents.back().reason == Net::DisconnectReason::VersionMismatch);
        CHECK(!Has(clientEvents, Net::EventKind::Connected));
        // Jamais connu du jeu côté serveur.
        CHECK(serverEvents.empty());
        CHECK(server.GetConnectionCount() == 0);
        CHECK(!client.IsConnected());
        return true;
    }

    bool ServerShutdownSeenByClient() {
        Net::Server server;
        CHECK(ListenOnFreePort(server, 1));
        Net::Client client;
        CHECK(client.Connect({.address = "127.0.0.1", .port = server.GetPort(), .gameVersion = 1}));
        std::vector<Received> serverEvents;
        std::vector<Received> clientEvents;
        CHECK(Pump(&server, &client, serverEvents, clientEvents, [&] { return client.IsConnected(); }));

        server.Close();
        CHECK(!server.IsListening());
        CHECK(Pump(nullptr, &client, serverEvents, clientEvents,
                   [&] { return Has(clientEvents, Net::EventKind::Disconnected); }));
        CHECK(clientEvents.back().reason == Net::DisconnectReason::ServerShutdown);
        CHECK(!client.IsConnected());
        return true;
    }

    bool BadAddressRejected() {
        Net::Client client;
        CHECK(!client.Connect({.address = "pas une adresse", .port = 1, .gameVersion = 1}));
        CHECK(client.Poll().empty());
        return true;
    }
}

bool RunNetTests() {
    return ConnectExchangeDisconnect() && OtherVersionRefused() && ServerShutdownSeenByClient() && BadAddressRejected();
}
