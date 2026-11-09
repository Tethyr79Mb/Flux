#pragma once

#include <cstdint>

namespace Game {
    // Version du protocole réseau du jeu (messages entre Ars Solaris et ArsSolarisServer), vérifiée par FluxNet à la
    // connexion : un client d'une autre version est refusé. Incrémenter à chaque changement d'un message du jeu ou de
    // ce qu'il transporte (composants répliqués, à partir de l'étape 6).
    inline constexpr std::uint32_t GameProtocolVersion = 2;

    // Premier octet de chaque message du jeu (FluxNet réserve 0). Les nombres sont en petit-boutiste (ByteStream.hpp).
    enum class MessageType : std::uint8_t {
        // Serveur -> client, fiable, juste après la connexion : empreinte des champs répliqués (u64, comparée par le
        // client à la sienne), scène à charger (texte, relative au dossier des assets), pas du serveur (u64).
        Welcome = 1,
        // Serveur -> client, non fiable, 30 par seconde : un instantané du monde (Replication::Encode).
        Snapshot = 2,
        // Client -> serveur, non fiable : numéro du dernier instantané reçu (u32), base des différences suivantes.
        SnapshotAck = 3,
        // Serveur -> client, fiable : une entité créée pendant le jeu, avec ses descendants (identifiant u64, texte de
        // SceneSerializer::SaveEntity). Ses champs répliqués suivent ensuite dans les instantanés.
        EntitySpawn = 4,
    };

    // Un instantané tous les SnapshotIntervalSteps pas de simulation (60 par seconde) : 30 par seconde.
    inline constexpr std::uint64_t SnapshotIntervalSteps = 2;

    // Port UDP du serveur quand rien d'autre n'est dit (--port du serveur, --serveur adresse:port du client).
    inline constexpr std::uint16_t DefaultServerPort = 27015;
}
