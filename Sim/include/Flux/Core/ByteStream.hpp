#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Flux {
    // Les valeurs passent telles qu'en mémoire : client et serveur sont en petit-boutiste (x86-64, ARM64).
    static_assert(std::endian::native == std::endian::little, "messages réseau écrits en petit-boutiste");

    // Écrit un message binaire (réseau) dans un tampon gardé d'un message à l'autre : Clear le vide sans rendre sa
    // mémoire, aucune allocation une fois la taille des messages atteinte.
    class ByteWriter {
        public:
        void Clear() { m_bytes.clear(); }
        std::span<const std::byte> GetBytes() const { return m_bytes; }
        std::size_t GetSize() const { return m_bytes.size(); }

        void WriteBytes(const void* data, std::size_t size) {
            const std::size_t start = m_bytes.size();
            m_bytes.resize(start + size);
            if (size > 0) {
                std::memcpy(m_bytes.data() + start, data, size);
            }
        }

        template <typename T>
            requires std::is_trivially_copyable_v<T>
        void Write(const T& value) {
            WriteBytes(&value, sizeof(T));
        }

        // Remplace une valeur déjà écrite (un nombre connu seulement à la fin, écrit d'abord à 0).
        template <typename T>
            requires std::is_trivially_copyable_v<T>
        void Patch(std::size_t offset, const T& value) {
            std::memcpy(m_bytes.data() + offset, &value, sizeof(T));
        }

        // Entier de 7 bits par octet (le bit du haut dit « encore un ») : 1 octet sous 128, au plus 10.
        void WriteVarint(std::uint64_t value) {
            while (value >= 0x80) {
                Write(static_cast<std::uint8_t>(value | 0x80));
                value >>= 7;
            }
            Write(static_cast<std::uint8_t>(value));
        }

        void WriteString(std::string_view text) {
            WriteVarint(text.size());
            WriteBytes(text.data(), text.size());
        }

        private:
        std::vector<std::byte> m_bytes;
    };

    // Lit un message écrit par ByteWriter. Un message tronqué ou abîmé ne fait jamais lire hors du tampon : la lecture
    // échoue (valeurs nulles) et IsValid devient faux, à vérifier à la fin.
    class ByteReader {
        public:
        explicit ByteReader(std::span<const std::byte> bytes) : m_bytes(bytes) {}

        bool IsValid() const { return m_valid; }
        bool IsAtEnd() const { return m_position == m_bytes.size(); }
        std::size_t GetRemaining() const { return m_bytes.size() - m_position; }
        // Lecture refusée (valeur hors limites trouvée par l'appelant) : IsValid devient faux.
        void Fail() { m_valid = false; }

        bool ReadBytes(void* data, std::size_t size) {
            if (!m_valid || size > GetRemaining()) {
                m_valid = false;
                std::memset(data, 0, size);
                return false;
            }
            if (size > 0) {
                std::memcpy(data, m_bytes.data() + m_position, size);
            }
            m_position += size;
            return true;
        }

        bool Skip(std::size_t size) {
            if (!m_valid || size > GetRemaining()) {
                m_valid = false;
                return false;
            }
            m_position += size;
            return true;
        }

        template <typename T>
            requires std::is_trivially_copyable_v<T>
        T Read() {
            T value;
            ReadBytes(&value, sizeof(T));
            return value;
        }

        std::uint64_t ReadVarint() {
            std::uint64_t value = 0;
            for (int shift = 0; shift < 64; shift += 7) {
                const auto byte = Read<std::uint8_t>();
                value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
                if ((byte & 0x80) == 0) {
                    return value;
                }
            }
            m_valid = false; // plus de 10 octets : abîmé
            return 0;
        }

        std::string ReadString() {
            const std::uint64_t size = ReadVarint();
            if (size > GetRemaining()) {
                m_valid = false;
                return {};
            }
            std::string text(static_cast<std::size_t>(size), '\0');
            ReadBytes(text.data(), text.size());
            return text;
        }

        private:
        std::span<const std::byte> m_bytes;
        std::size_t m_position = 0;
        bool m_valid = true;
    };
}
