#include "Flux/Scene/PlayerCommand.hpp"

#include <algorithm>
#include <cmath>

namespace Flux {
    namespace {
        static_assert(MaxInputActions == 128, "WriteCommand écrit les bits en deux mots de 64");

        void WriteBits(ByteWriter& writer, const std::bitset<MaxInputActions>& bits) {
            const std::bitset<MaxInputActions> low = bits & std::bitset<MaxInputActions>(~0ull);
            writer.Write(static_cast<std::uint64_t>(low.to_ullong()));
            writer.Write(static_cast<std::uint64_t>((bits >> 64).to_ullong()));
        }

        std::bitset<MaxInputActions> ReadBits(ByteReader& reader) {
            const auto low = reader.Read<std::uint64_t>();
            const auto high = reader.Read<std::uint64_t>();
            return (std::bitset<MaxInputActions>(high) << 64) | std::bitset<MaxInputActions>(low);
        }

        float Clean(float value, float limit) {
            return std::isfinite(value) ? std::clamp(value, -limit, limit) : 0.0f;
        }

        Math::Vector3f Clean(Math::Vector3f vector, float limit) {
            return {Clean(vector.x, limit), Clean(vector.y, limit), Clean(vector.z, limit)};
        }
    }

    void WriteCommand(ByteWriter& writer, const PlayerCommand& command) {
        writer.Write(command.step);
        writer.Write(command.move);
        writer.Write(command.stickYaw);
        writer.Write(command.stickPitch);
        writer.Write(command.viewPosition.sector);
        writer.Write(command.viewPosition.offset);
        writer.Write(command.viewForward);
        WriteBits(writer, command.held);
        WriteBits(writer, command.pressed);
    }

    bool ReadCommand(ByteReader& reader, PlayerCommand& command) {
        PlayerCommand read;
        read.step = reader.Read<std::uint64_t>();
        read.move = Clean(reader.Read<Math::Vector3f>(), 1.0f);
        read.stickYaw = Clean(reader.Read<float>(), 1.0f);
        read.stickPitch = Clean(reader.Read<float>(), 1.0f);
        read.viewPosition.sector = reader.Read<Math::Vector3i64>();
        read.viewPosition.offset = reader.Read<Math::Vector3d>();
        read.viewForward = Clean(reader.Read<Math::Vector3f>(), 1.0f);
        read.held = ReadBits(reader);
        read.pressed = ReadBits(reader);
        if (!reader.IsValid()) {
            return false;
        }
        if (const float length = Math::Length(read.move); length > 1.0f) {
            read.move = read.move * (1.0f / length);
        }
        const double offset = Math::Length(read.viewPosition.offset);
        if (!std::isfinite(offset) || offset > 2.0 * Math::WorldPosition::SectorSize) {
            read.viewPosition = {}; // vue hors de son secteur : ignorée (visée au centre du secteur)
        }
        if (const float length = Math::Length(read.viewForward); length > 1.0e-3f) {
            read.viewForward = read.viewForward * (1.0f / length);
        } else {
            read.viewForward = PlayerCommand{}.viewForward;
        }
        command = read;
        return true;
    }

    InputActionId CommandActions::Declare(std::string_view name) {
        if (const InputActionId id = GetId(name); id != NoInputAction) {
            return id;
        }
        if (m_ids.size() >= MaxInputActions) {
            return NoInputAction;
        }
        const auto id = static_cast<InputActionId>(m_ids.size());
        m_ids.emplace(std::string(name), id);
        return id;
    }

    InputActionId CommandActions::GetId(std::string_view name) const {
        const auto found = m_ids.find(name);
        return found != m_ids.end() ? found->second : NoInputAction;
    }
}
