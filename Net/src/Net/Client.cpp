#include "Flux/Net/Client.hpp"

#include "Net/Handshake.hpp"
#include "Net/Endpoint.hpp"

#include "Flux/Core/Log.hpp"

#include <format>

namespace Flux::Net {
    namespace {
        constexpr int ReceiveBatch = 64;
    }

    struct Client::Impl final : Detail::Endpoint {
        ISteamNetworkingSockets* sockets = nullptr;
        HSteamNetConnection connection = k_HSteamNetConnection_Invalid;
        Detail::Versions versions;
        bool helloSent = false;
        bool welcomed = false;
        // En dernier, comme pour Server::Impl.
        Detail::LibraryRef library{*this};

        void Forget(DisconnectReason reason) {
            sockets->CloseConnection(connection, 0, nullptr, false);
            connection = k_HSteamNetConnection_Invalid;
            helloSent = false;
            welcomed = false;
            Push(EventKind::Disconnected, 0, reason);
        }

        void ProcessStatus(const SteamNetConnectionStatusChangedCallback_t& info) {
            if (info.m_hConn != connection) {
                return;
            }
            switch (info.m_info.m_eState) {
                case k_ESteamNetworkingConnectionState_Connected:
                    if (!helloSent) {
                        const Detail::ControlMessage hello =
                            Detail::EncodeControl(Detail::ControlType::Hello, versions);
                        sockets->SendMessageToConnection(connection, hello.data(),
                                                         static_cast<std::uint32_t>(hello.size()),
                                                         k_nSteamNetworkingSend_Reliable, nullptr);
                        helloSent = true;
                    }
                    return;
                case k_ESteamNetworkingConnectionState_ClosedByPeer:
                case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
                    const DisconnectReason reason = Detail::ToDisconnectReason(info.m_info);
                    if (reason == DisconnectReason::VersionMismatch) {
                        Log::Warning("réseau : connexion refusée, versions différentes ({})", info.m_info.m_szEndDebug);
                    }
                    Forget(reason);
                    return;
                }
                default: return;
            }
        }

        void Receive() {
            SteamNetworkingMessage_t* batch[ReceiveBatch];
            for (;;) {
                if (connection == k_HSteamNetConnection_Invalid) {
                    return;
                }
                const int count = sockets->ReceiveMessagesOnConnection(connection, batch, ReceiveBatch);
                for (int i = 0; i < count; ++i) {
                    SteamNetworkingMessage_t* message = batch[i];
                    const std::span<const std::byte> payload = Detail::GetPayload(*message);
                    if (payload.empty() || payload[0] == ReservedMessageType) {
                        if (!welcomed && Detail::DecodeControl(Detail::ControlType::Welcome, payload)) {
                            welcomed = true;
                            Push(EventKind::Connected, 0);
                        }
                        message->Release();
                    } else if (welcomed) {
                        PushMessage(0, message);
                    } else {
                        // Un message du jeu envoyé sans fiabilité peut dépasser l'accueil (Welcome) retransmis.
                        message->Release();
                    }
                }
                if (count < ReceiveBatch) {
                    return;
                }
            }
        }

        void Close(int reason, const char* detail) {
            if (connection == k_HSteamNetConnection_Invalid) {
                return;
            }
            ReleaseHeldMessages();
            sockets->CloseConnection(connection, reason, detail, true);
            connection = k_HSteamNetConnection_Invalid;
            helloSent = false;
            welcomed = false;
        }
    };

    Client::Client() : m_impl(std::make_unique<Impl>()) {}

    Client::~Client() {
        m_impl->Close(Detail::EndClosed, "client fermé");
    }

    std::expected<void, std::string> Client::Connect(const ClientConfig& config) {
        if (!m_impl->library.GetStatus()) {
            return std::unexpected(m_impl->library.GetStatus().error());
        }
        m_impl->sockets = SteamNetworkingSockets();
        m_impl->Close(Detail::EndClosed, "nouvelle connexion");
        m_impl->versions = {ProtocolVersion, config.gameVersion};

        SteamNetworkingIPAddr address;
        if (!address.ParseString(config.address.c_str())) {
            return std::unexpected(std::format("adresse IP invalide : « {} »", config.address));
        }
        address.m_port = config.port;
        SteamNetworkingConfigValue_t options[2];
        m_impl->library.FillOptions(options);
        m_impl->connection = m_impl->sockets->ConnectByIPAddress(address, 2, options);
        if (m_impl->connection == k_HSteamNetConnection_Invalid) {
            return std::unexpected(std::format("connexion impossible vers {}:{}", config.address, config.port));
        }
        return {};
    }

    void Client::Disconnect() {
        m_impl->Close(Detail::EndClosed, "déconnexion");
    }

    bool Client::IsConnected() const {
        return m_impl->welcomed;
    }

    std::span<const Event> Client::Poll() {
        Impl& impl = *m_impl;
        if (impl.sockets == nullptr) {
            return {};
        }
        for (const SteamNetConnectionStatusChangedCallback_t& status : impl.BeginPoll(*impl.sockets)) {
            impl.ProcessStatus(status);
        }
        impl.Receive();
        return impl.m_events;
    }

    bool Client::Send(std::span<const std::byte> payload, Delivery delivery) {
        if (!m_impl->welcomed || payload.empty() || payload[0] == ReservedMessageType) {
            return false;
        }
        return m_impl->sockets->SendMessageToConnection(m_impl->connection, payload.data(),
                                                        static_cast<std::uint32_t>(payload.size()),
                                                        Detail::ToSendFlags(delivery),
                                                        nullptr) == k_EResultOK;
    }

    void Client::Flush() {
        if (m_impl->connection != k_HSteamNetConnection_Invalid) {
            m_impl->sockets->FlushMessagesOnConnection(m_impl->connection);
        }
    }
}
