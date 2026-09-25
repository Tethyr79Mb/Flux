#include "Flux/Core/Uuid.hpp"

#include <charconv>
#include <format>
#include <random>

namespace Flux {
    Uuid Uuid::Generate() {
        // Un générateur par thread, initialisé une fois par le hasard du système.
        thread_local std::mt19937_64 generator{[] {
            std::random_device device;
            return (static_cast<std::uint64_t>(device()) << 32) ^ device();
        }()};
        std::uint64_t value = 0;
        while (value == 0) {
            value = generator();
        }
        return {value};
    }

    std::string Uuid::ToString() const {
        return std::format("{:016x}", value);
    }

    std::optional<Uuid> Uuid::Parse(std::string_view text) {
        std::uint64_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
        if (error != std::errc{} || end != text.data() + text.size() || text.size() != 16 || value == 0) {
            return std::nullopt;
        }
        return Uuid{value};
    }
}
