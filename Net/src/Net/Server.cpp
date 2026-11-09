#include "Flux/Net/Server.hpp"

#include "Net/Handshake.hpp"
#include "Net/Endpoint.hpp"

#include "Flux/Core/Log.hpp"

#include <chrono>
#include <format>
#include <unordered_map>

namespace Flux::Net {
    namespace {
        using Clock = std::chrono::steady_clock;
        // Temps laissé à un client pour envoyer sa poignée de main après l'ouverture de la connexion.
        constexpr auto HandshakeTimeout = std::chrono::seconds(5);
        constexpr int ReceiveBatch = 64;

        struct Peer {
            bool welcomed = false; // poignée de main réussie : connue du jeu
            Clock::time_point acceptedAt;
        };
    }

    struct Server::Impl final : Detail::Endpoint {
        ISteamNetworkingSockets* sockets = nullptr;
        HSteamListenSocket listenSocket = k_HSteamListenSocket_Invalid;
        HSteamNetPollGroup pollGroup = k_HSteamNetPollGroup_Invalid;
        Detail::Versions versions;
        std::unordered_map<HSteamNetConnection, Peer> peers;
        std::size_t welcomedCount = 0;

        // En dernier : détruit le premier, il désinscrit ce serveur avant que le reste disparaisse (un rappel venu d'un
        // autre fil ne touche plus le verrou ni les listes).
        Detail::LibraryRef library{*this};

        void Drop(HSteamNetConnection connection, int reason, const char* detail) {
            sockets->CloseConnection(connection, reason, detail, false);
            const auto it = peers.find(connection);
            if (it == peers.end()) {
                return;
            }
            if (it->second.welcomed) {
                --welcomedCount;
            }
            peers.erase(it);
        }

        void ProcessStatus(const SteamNetConnectionStatusChangedCallback_t& info) {
            const HSteamNetConnection connection = info.m_hConn;
            switch (info.m_info.m_eState) {
                case k_ESteamNetworkingConnectionState_Connecting:
                    if (info.m_info.m_hListenSocket != listenSocket || peers.contains(connection)) {
                        return;
                    }
                    if (sockets->AcceptConnection(connection) != k_EResultOK) {
                        sockets->CloseConnection(connection, 0, nullptr, false);
                        return;
                    }
                    sockets->SetConnectionPollGroup(connection, pollGroup);
                    peers.emplace(connection, Peer{false, Clock::now()});
                    return;
                case k_ESteamNetworkingConnectionState_ClosedByPeer:
                case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
                    const auto it = peers.find(connection);
                    if (it == peers.end()) {
                        sockets->CloseConnection(connection, 0, nullptr, false);
                        return;
                    }
                    if (it->second.welcomed) {
                        Push(EventKind::Disconnected, connection, Detail::ToDisconnectReason(info.m_info));
                    }
                    Drop(connection, 0, nullptr);
                    return;
                }
                default: return;
            }
        }

        void ReceiveHello(HSteamNetConnection connection, Peer& peer, std::span<const std::byte> message) {
            const std::optional<Detail::Versions> client = Detail::DecodeControl(Detail::ControlType::Hello, message);
            if (!client) {
                Drop(connection, Detail::EndHandshakeFailed, "poignée de main mal formée");
                return;
            }
            if (*client != versions) {
                const std::string detail = std::format("client FluxNet {} jeu {}, serveur FluxNet {} jeu {}",
                                                       client->net, client->game, versions.net, versions.game);
                Log::Warning("réseau : connexion {} refusée, versions différentes ({})", connection, detail);
                Drop(connection, Detail::EndVersionMismatch, detail.c_str());
                return;
            }
            const Detail::ControlMessage welcome = Detail::EncodeControl(Detail::ControlType::Welcome, versions);
            sockets->SendMessageToConnection(connection, welcome.data(), static_cast<std::uint32_t>(welcome.size()),
                                             k_nSteamNetworkingSend_Reliable, nullptr);
            peer.welcomed = true;
            ++welcomedCount;
            Push(EventKind::Connected, connection);
        }

        void Receive() {
            SteamNetworkingMessage_t* batch[ReceiveBatch];
            for (;;) {
                const int count = sockets->ReceiveMessagesOnPollGroup(pollGroup, batch, ReceiveBatch);
                for (int i = 0; i < count; ++i) {
                    SteamNetworkingMessage_t* message = batch[i];
                    const std::span<const std::byte> payload = Detail::GetPayload(*message);
                    const auto it = peers.find(message->GetConnection());
                    if (it == peers.end()) {
                        message->Release();
                        continue;
                    }
                    if (!it->second.welcomed) {
                        ReceiveHello(message->GetConnection(), it->second, payload);
                        message->Release();
                    } else if (payload.empty() || payload[0] == ReservedMessageType) {
                        // Pas de message de contrôle après la poignée de main pour l'instant.
                        message->Release();
                    } else {
                        PushMessage(message->GetConnection(), message);
                    }
                }
                if (count < ReceiveBatch) {
                    return;
                }
            }
        }

        void DropSlowHandshakes() {
            if (peers.size() == welcomedCount) {
                return; // aucune poignée de main en cours : rien à parcourir à chaque pas
            }
            const Clock::time_point now = Clock::now();
            for (auto it = peers.begin(); it != peers.end();) {
                if (!it->second.welcomed && now - it->second.acceptedAt > HandshakeTimeout) {
                    sockets->CloseConnection(it->first, Detail::EndHandshakeFailed, "poignée de main trop lente",
                                             false);
                    it = peers.erase(it);
                } else {
                    ++it;
                }
            }
        }

        void Close() {
            if (listenSocket == k_HSteamListenSocket_Invalid) {
                return;
            }
            ReleaseHeldMessages();
            // Sans attente (linger) : fermer la prise d'écoute couperait brutalement une connexion qui attend encore,
            // et le client ne saurait pas que le serveur s'arrête avant d'avoir attendu la fin du délai.
            for (const auto& [connection, peer] : peers) {
                sockets->CloseConnection(connection, Detail::EndServerShutdown, "arrêt du serveur", false);
            }
            peers.clear();
            welcomedCount = 0;
            sockets->CloseListenSocket(listenSocket);
            sockets->DestroyPollGroup(pollGroup);
            listenSocket = k_HSteamListenSocket_Invalid;
            pollGroup = k_HSteamNetPollGroup_Invalid;
            m_events.clear();
        }
    };

    Server::Server() : m_impl(std::make_unique<Impl>()) {}

    Server::~Server() {
        m_impl->Close();
    }

    std::expected<void, std::string> Server::Listen(const ServerConfig& config) {
        if (!m_impl->library.GetStatus()) {
            return std::unexpected(m_impl->library.GetStatus().error());
        }
        m_impl->Close();
        m_impl->sockets = SteamNetworkingSockets();
        m_impl->versions = {ProtocolVersion, config.gameVersion};

        SteamNetworkingIPAddr address;
        address.Clear(); // toutes les interfaces, IPv4 et IPv6
        address.m_port = config.port;
        SteamNetworkingConfigValue_t options[2];
        m_impl->library.FillOptions(options);
        m_impl->listenSocket = m_impl->sockets->CreateListenSocketIP(address, 2, options);
        if (m_impl->listenSocket == k_HSteamListenSocket_Invalid) {
            return std::unexpected(std::format("impossible d'écouter le port UDP {}", config.port));
        }
        m_impl->pollGroup = m_impl->sockets->CreatePollGroup();
        return {};
    }

    void Server::Close() {
        m_impl->Close();
    }

    bool Server::IsListening() const {
        return m_impl->listenSocket != k_HSteamListenSocket_Invalid;
    }

    std::uint16_t Server::GetPort() const {
        SteamNetworkingIPAddr address;
        if (!IsListening() || !m_impl->sockets->GetListenSocketAddress(m_impl->listenSocket, &address)) {
            return 0;
        }
        return address.m_port;
    }

    std::span<const Event> Server::Poll() {
        Impl& impl = *m_impl;
        if (!IsListening()) {
            return {};
        }
        for (const SteamNetConnectionStatusChangedCallback_t& status : impl.BeginPoll(*impl.sockets)) {
            impl.ProcessStatus(status);
        }
        impl.Receive();
        impl.DropSlowHandshakes();
        return impl.m_events;
    }

    bool Server::Send(ConnectionId connection, std::span<const std::byte> payload, Delivery delivery) {
        if (payload.empty() || payload[0] == ReservedMessageType) {
            return false;
        }
        const auto it = m_impl->peers.find(connection);
        if (it == m_impl->peers.end() || !it->second.welcomed) {
            return false;
        }
        return m_impl->sockets->SendMessageToConnection(connection, payload.data(),
                                                        static_cast<std::uint32_t>(payload.size()),
                                                        Detail::ToSendFlags(delivery, payload.size()),
                                                        nullptr) == k_EResultOK;
    }

    void Server::Flush() {
        for (const auto& [connection, peer] : m_impl->peers) {
            if (peer.welcomed) {
                m_impl->sockets->FlushMessagesOnConnection(connection);
            }
        }
    }

    void Server::Disconnect(ConnectionId connection) {
        if (m_impl->peers.contains(connection)) {
            m_impl->Drop(connection, Detail::EndClosed, "déconnecté par le serveur");
        }
    }

    std::size_t Server::GetConnectionCount() const {
        return m_impl->welcomedCount;
    }
}
