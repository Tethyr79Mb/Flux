#pragma once

#include "Flux/NetExport.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace Flux::Net {
    // Version du protocole de FluxNet lui-même (poignée de main, messages de contrôle). Le client et le serveur
    // doivent avoir la même, comme la même version du protocole du jeu (donnée par le jeu) : sinon la connexion est
    // refusée dès la poignée de main, avec la raison VersionMismatch. Incrémenter à chaque changement du format.
    inline constexpr std::uint32_t ProtocolVersion = 1;

    // Une connexion, du point de vue du serveur (le client n'en a qu'une). 0 : aucune.
    using ConnectionId = std::uint32_t;

    enum class Delivery : std::uint8_t {
        Reliable,   // arrive, dans l'ordre des envois fiables (commandes, événements)
        Unreliable, // peut se perdre ou arriver avant un envoi fiable plus ancien (état répété à chaque envoi)
    };

    enum class DisconnectReason : std::uint8_t {
        Closed,          // fermée par l'autre côté (déconnexion normale)
        ServerShutdown,  // le serveur s'arrête
        VersionMismatch, // versions du protocole différentes (FluxNet ou jeu) : mettre à jour le client
        HandshakeFailed, // poignée de main absente, trop lente ou mal formée
        Lost,            // plus de réponse, ou serveur injoignable
    };

    enum class EventKind : std::uint8_t {
        Connected,    // poignée de main réussie : les messages du jeu peuvent passer
        Disconnected, // reason dit pourquoi ; la connexion n'existe plus
        Message,      // payload : un message du jeu
    };

    struct Event {
        EventKind kind = EventKind::Message;
        ConnectionId connection = 0;
        DisconnectReason reason = DisconnectReason::Closed;
        // Message : valable jusqu'au Poll suivant. Le premier octet est le type du message, choisi par le jeu.
        std::span<const std::byte> payload;
    };

    // Le premier octet de chaque message est son type. Le type 0 est réservé à FluxNet (poignée de main) : les messages
    // du jeu commencent par 1 à 255 et FluxNet ne leur ajoute aucun octet.
    inline constexpr std::byte ReservedMessageType{0};

    // En français, pour le journal.
    FLUX_NET_API const char* ToString(DisconnectReason reason);
}
