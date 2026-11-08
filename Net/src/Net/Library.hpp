#pragma once

// Détail de FluxNet : la bibliothèque GameNetworkingSockets (initialisée une fois par processus tant qu'un serveur ou
// un client existe) et la distribution de ses changements d'état de connexion.
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include "Flux/Net/NetTypes.hpp"

#include <cstdint>
#include <expected>
#include <string>

namespace Flux::Net::Detail {
    // Reçoit les changements d'état de ses connexions. Appelé pendant RunCallbacks, qui peut venir du Poll d'un autre
    // serveur ou client du processus, donc d'un autre fil : l'implémentation range l'information sous verrou et la
    // traite dans son propre Poll.
    class StatusListener {
        public:
        virtual void OnStatusChanged(const SteamNetConnectionStatusChangedCallback_t& info) = 0;

        protected:
        ~StatusListener() = default;
    };

    // Une référence à la bibliothèque : initialisée à la première, arrêtée à la dernière. listener est inscrit sous un
    // jeton (GetToken), à mettre dans ConnectionUserData des connexions : un rappel arrivé après la destruction du
    // propriétaire ne trouve plus personne, au lieu d'un pointeur pendant.
    class LibraryRef {
        public:
        explicit LibraryRef(StatusListener& listener);
        ~LibraryRef();

        LibraryRef(const LibraryRef&) = delete;
        LibraryRef& operator=(const LibraryRef&) = delete;

        // Erreur si GameNetworkingSockets n'a pas pu démarrer.
        const std::expected<void, std::string>& GetStatus() const { return m_status; }
        std::int64_t GetToken() const { return m_token; }

        // Options de connexion communes : rappel d'état et jeton du propriétaire.
        void FillOptions(SteamNetworkingConfigValue_t (&options)[2]) const;

        private:
        std::expected<void, std::string> m_status;
        std::int64_t m_token = 0;
    };

    // Raisons de fin choisies par FluxNet (m_eEndReason), dans la plage réservée aux applications.
    enum EndReason : int {
        EndClosed = k_ESteamNetConnectionEnd_App_Min,
        EndServerShutdown,
        EndVersionMismatch,
        EndHandshakeFailed,
    };
    DisconnectReason ToDisconnectReason(const SteamNetConnectionInfo_t& info);
}
