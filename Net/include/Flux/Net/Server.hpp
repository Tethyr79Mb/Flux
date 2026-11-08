#pragma once

#include "Flux/NetExport.hpp"
#include "Flux/Net/NetTypes.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

namespace Flux::Net {
    struct ServerConfig {
        std::uint16_t port = 0;         // obligatoire (GameNetworkingSockets ne laisse pas le système choisir)
        std::uint32_t gameVersion = 0;  // version du protocole du jeu, comparée à celle de chaque client
    };

    // Écoute les clients en UDP (GameNetworkingSockets : chiffré, fiable ou non, en messages). Un client n'apparaît
    // (Connected) qu'après une poignée de main dont les versions concordent. Utilisé depuis un seul fil ; un client et
    // un serveur du même processus peuvent être sur des fils différents.
    class FLUX_NET_API Server {
        public:
        Server();
        ~Server(); // ferme toutes les connexions (ServerShutdown)

        Server(const Server&) = delete;
        Server& operator=(const Server&) = delete;

        std::expected<void, std::string> Listen(const ServerConfig& config);
        void Close();
        bool IsListening() const;
        std::uint16_t GetPort() const;

        // Événements arrivés depuis l'appel précédent ; valables jusqu'au suivant. Une fois par pas de simulation.
        std::span<const Event> Poll();

        // payload[0] : type du message du jeu, jamais ReservedMessageType. Faux si la connexion n'est pas établie.
        bool Send(ConnectionId connection, std::span<const std::byte> payload, Delivery delivery);
        // Part tout de suite au lieu d'attendre d'être regroupé avec les suivants (fin d'un pas).
        void Flush();
        void Disconnect(ConnectionId connection);
        std::size_t GetConnectionCount() const;

        private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
