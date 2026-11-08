#include "Net/Library.hpp"

#include "Flux/Core/Log.hpp"

#include <mutex>
#include <unordered_map>

namespace Flux::Net {
    const char* ToString(DisconnectReason reason) {
        switch (reason) {
            case DisconnectReason::Closed: return "fermée";
            case DisconnectReason::ServerShutdown: return "arrêt du serveur";
            case DisconnectReason::VersionMismatch: return "versions différentes";
            case DisconnectReason::HandshakeFailed: return "poignée de main ratée";
            case DisconnectReason::Lost: return "perdue";
        }
        return "?";
    }
}

namespace Flux::Net::Detail {
    namespace {
        // Un seul état pour le processus : GameNetworkingSockets lui-même n'en a qu'un.
        struct LibraryState {
            std::mutex mutex;
            int references = 0;
            std::int64_t nextToken = 1;
            std::unordered_map<std::int64_t, StatusListener*> listeners;
        };

        LibraryState& GetState() {
            static LibraryState state;
            return state;
        }

        void OnStatusChanged(SteamNetConnectionStatusChangedCallback_t* info) {
            LibraryState& state = GetState();
            // Verrou gardé pendant l'appel : un propriétaire ne peut pas être détruit pendant qu'on lui parle.
            std::scoped_lock lock(state.mutex);
            const auto it = state.listeners.find(info->m_info.m_nUserData);
            if (it != state.listeners.end()) {
                it->second->OnStatusChanged(*info);
            }
        }

        void OnDebugOutput(ESteamNetworkingSocketsDebugOutputType type, const char* message) {
            if (type <= k_ESteamNetworkingSocketsDebugOutputType_Error) {
                Log::Error("réseau : {}", message);
            } else {
                Log::Warning("réseau : {}", message);
            }
        }
    }

    LibraryRef::LibraryRef(StatusListener& listener) {
        LibraryState& state = GetState();
        std::scoped_lock lock(state.mutex);
        if (state.references == 0) {
            SteamNetworkingErrMsg error{};
            if (!GameNetworkingSockets_Init(nullptr, error)) {
                m_status = std::unexpected(std::string("GameNetworkingSockets ne démarre pas : ") + error);
                return;
            }
            SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Warning,
                                                           OnDebugOutput);
        }
        ++state.references;
        m_token = state.nextToken++;
        state.listeners.emplace(m_token, &listener);
    }

    LibraryRef::~LibraryRef() {
        if (!m_status) {
            return;
        }
        LibraryState& state = GetState();
        std::scoped_lock lock(state.mutex);
        state.listeners.erase(m_token);
        if (--state.references == 0) {
            GameNetworkingSockets_Kill();
        }
    }

    void LibraryRef::FillOptions(SteamNetworkingConfigValue_t (&options)[2]) const {
        options[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                          reinterpret_cast<void*>(&OnStatusChanged));
        options[1].SetInt64(k_ESteamNetworkingConfig_ConnectionUserData, m_token);
    }

    DisconnectReason ToDisconnectReason(const SteamNetConnectionInfo_t& info) {
        switch (info.m_eEndReason) {
            case EndClosed: return DisconnectReason::Closed;
            case EndServerShutdown: return DisconnectReason::ServerShutdown;
            case EndVersionMismatch: return DisconnectReason::VersionMismatch;
            case EndHandshakeFailed: return DisconnectReason::HandshakeFailed;
            default: break;
        }
        // Fermée par l'autre côté sans raison de FluxNet (ancien client, fermeture brutale) : fermeture normale.
        return info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ? DisconnectReason::Closed
                                                                                : DisconnectReason::Lost;
    }
}
