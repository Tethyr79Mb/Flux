#pragma once

// Détail de FluxNet : ce que le serveur et le client partagent (changements d'état rangés sous verrou, événements et
// messages gardés jusqu'au Poll suivant).
#include "Net/Library.hpp"

#include <mutex>
#include <utility>
#include <vector>

namespace Flux::Net::Detail {
    class Endpoint : public StatusListener {
        public:
        void OnStatusChanged(const SteamNetConnectionStatusChangedCallback_t& info) final {
            std::scoped_lock lock(m_statusMutex);
            m_pendingStatus.push_back(info);
        }

        // Début d'un Poll : rend les messages du précédent et les changements d'état arrivés depuis.
        const std::vector<SteamNetConnectionStatusChangedCallback_t>& BeginPoll(ISteamNetworkingSockets& sockets) {
            ReleaseHeldMessages();
            m_events.clear();
            m_statusToProcess.clear();
            sockets.RunCallbacks();
            std::scoped_lock lock(m_statusMutex);
            std::swap(m_pendingStatus, m_statusToProcess);
            return m_statusToProcess;
        }

        void ReleaseHeldMessages() {
            for (SteamNetworkingMessage_t* message : m_heldMessages) {
                message->Release();
            }
            m_heldMessages.clear();
        }

        void Push(EventKind kind, ConnectionId connection, DisconnectReason reason = DisconnectReason::Closed) {
            m_events.push_back({kind, connection, reason, {}});
        }

        // Le message reste à FluxNet jusqu'au Poll suivant : le jeu le lit sans copie.
        void PushMessage(ConnectionId connection, SteamNetworkingMessage_t* message) {
            m_heldMessages.push_back(message);
            m_events.push_back({EventKind::Message, connection, DisconnectReason::Closed,
                                {static_cast<const std::byte*>(message->GetData()),
                                 static_cast<std::size_t>(message->GetSize())}});
        }

        std::vector<Event> m_events;

        protected:
        ~Endpoint() = default;

        private:
        // Rangés par OnStatusChanged (pendant RunCallbacks, de n'importe quel fil), traités par Poll.
        std::mutex m_statusMutex;
        std::vector<SteamNetConnectionStatusChangedCallback_t> m_pendingStatus;
        std::vector<SteamNetConnectionStatusChangedCallback_t> m_statusToProcess;
        std::vector<SteamNetworkingMessage_t*> m_heldMessages;
    };

    inline std::span<const std::byte> GetPayload(const SteamNetworkingMessage_t& message) {
        return {static_cast<const std::byte*>(message.GetData()), static_cast<std::size_t>(message.GetSize())};
    }

    inline int ToSendFlags(Delivery delivery) {
        return delivery == Delivery::Reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
    }
}
