#pragma once

#include "Flux/Export.hpp"

#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Flux {
    // Identifiant aléatoire de 64 bits, stable d'une sauvegarde à l'autre (contrairement aux Entity d'EnTT).
    // 0 signifie « aucun ». Collision négligeable : une chance sur 10^7 pour un million d'entités.
    struct FLUX_API Uuid
    {
        std::uint64_t value = 0;

        static Uuid Generate();

        // 16 chiffres hexadécimaux : un nombre JSON perdrait les bits au-delà de 2^53 dans d'autres outils.
        std::string ToString() const;
        static std::optional<Uuid> Parse(std::string_view text);

        explicit operator bool() const { return value != 0; }
        auto operator<=>(const Uuid&) const = default;
    };
}

template <>
struct std::hash<Flux::Uuid> {
    std::size_t operator()(const Flux::Uuid& uuid) const noexcept { return std::hash<std::uint64_t>{}(uuid.value); }
};
