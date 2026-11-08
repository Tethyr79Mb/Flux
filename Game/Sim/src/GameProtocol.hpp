#pragma once

#include <cstdint>

namespace Game {
    // Version du protocole réseau du jeu (messages entre Ars Solaris et ArsSolarisServer), vérifiée par FluxNet à la
    // connexion : un client d'une autre version est refusé. Incrémenter à chaque changement d'un message du jeu ou de
    // ce qu'il transporte (composants répliqués, à partir de l'étape 6).
    inline constexpr std::uint32_t GameProtocolVersion = 1;

    // Port UDP du serveur quand rien d'autre n'est dit (--port du serveur, --serveur adresse:port du client).
    inline constexpr std::uint16_t DefaultServerPort = 27015;
}
