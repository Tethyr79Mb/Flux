#pragma once

// Messages de contrôle de FluxNet (premier octet ReservedMessageType). Entiers en petit-boutiste, quelle que soit la
// machine : le format ne dépend pas du processeur du serveur.
#include "Flux/Net/NetTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Flux::Net::Detail {
    enum class ControlType : std::uint8_t {
        Hello = 1,   // client -> serveur : ses versions
        Welcome = 2, // serveur -> client : accepté, avec les versions du serveur
    };

    struct Versions {
        std::uint32_t net = ProtocolVersion;
        std::uint32_t game = 0;

        bool operator==(const Versions&) const = default;
    };

    inline constexpr std::size_t ControlMessageSize = 2 + 4 + 4;
    using ControlMessage = std::array<std::byte, ControlMessageSize>;

    inline ControlMessage EncodeControl(ControlType type, const Versions& versions) {
        ControlMessage message{};
        message[0] = ReservedMessageType;
        message[1] = static_cast<std::byte>(type);
        for (int i = 0; i < 4; ++i) {
            message[2 + i] = static_cast<std::byte>(versions.net >> (8 * i));
            message[6 + i] = static_cast<std::byte>(versions.game >> (8 * i));
        }
        return message;
    }

    // Vide si ce n'est pas un message de contrôle de ce type, bien formé.
    inline std::optional<Versions> DecodeControl(ControlType type, std::span<const std::byte> message) {
        if (message.size() != ControlMessageSize || message[0] != ReservedMessageType ||
            message[1] != static_cast<std::byte>(type)) {
            return std::nullopt;
        }
        Versions versions{0, 0};
        for (int i = 0; i < 4; ++i) {
            versions.net |= std::to_integer<std::uint32_t>(message[2 + i]) << (8 * i);
            versions.game |= std::to_integer<std::uint32_t>(message[6 + i]) << (8 * i);
        }
        return versions;
    }
}
