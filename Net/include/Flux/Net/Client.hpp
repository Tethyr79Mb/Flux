#pragma once

#include "Flux/NetExport.hpp"
#include "Flux/Net/NetTypes.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

namespace Flux::Net {
    struct ClientConfig {
        std::string address;            // adresse IP du serveur (IPv4 ou IPv6), sans nom de domaine pour l'instant
        std::uint16_t port = 0;
        std::uint32_t gameVersion = 0;  // version du protocole du jeu, vérifiée par le serveur
    };

    // Une connexion à un serveur. Connect revient tout de suite : Poll rend ensuite Connected (poignée de main
    // acceptée) ou Disconnected (raison : versions différentes, serveur injoignable…).
    class FLUX_NET_API Client {
        public:
        Client();
        ~Client(); // déconnecte (Closed côté serveur)

        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;

        std::expected<void, std::string> Connect(const ClientConfig& config);
        void Disconnect();
        // Vrai entre Connected et Disconnected.
        bool IsConnected() const;

        // Événements arrivés depuis l'appel précédent ; valables jusqu'au suivant. Une fois par pas de simulation.
        std::span<const Event> Poll();

        // payload[0] : type du message du jeu, jamais ReservedMessageType. Faux si pas connecté.
        bool Send(std::span<const std::byte> payload, Delivery delivery);
        void Flush();

        private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
