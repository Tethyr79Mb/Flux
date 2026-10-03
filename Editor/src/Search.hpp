#pragma once

#include <string>
#include <string_view>

namespace FluxEditor {
    // Recherche dans les listes de l'éditeur (objets, fichiers, messages) : sans tenir compte des majuscules ni des
    // accents usuels du français (« etoile » trouve « Étoile »). Le texte cherché est préparé une fois, puis comparé
    // à chaque ligne.
    class SearchQuery {
        public:
        explicit SearchQuery(std::string_view query) : m_folded(Fold(query)) {}

        bool IsEmpty() const { return m_folded.empty(); }
        // Vrai si text contient la recherche (toujours vrai pour une recherche vide).
        bool Matches(std::string_view text) const {
            return m_folded.empty() || Fold(text).find(m_folded) != std::string::npos;
        }

        // Minuscules ASCII ; lettres accentuées du Latin-1 (UTF-8 sur deux octets, C3 xx) ramenées à leur lettre
        // de base ; le reste est gardé tel quel.
        static std::string Fold(std::string_view text) {
            std::string folded;
            folded.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i) {
                const auto c = static_cast<unsigned char>(text[i]);
                if (c >= 'A' && c <= 'Z') {
                    folded += static_cast<char>(c - 'A' + 'a');
                } else if (c == 0xC3 && i + 1 < text.size()) {
                    // De U+00C0 (À) à U+00FF (ÿ) : l'octet suivant moins 0x80 indexe cette table (0 : non converti).
                    using namespace std::string_view_literals; // « sv » : la table garde ses \0 (sinon coupée au premier)
                    constexpr std::string_view Base = "aaaaaaaceeeeiiii\0nooooo\0ouuuuy\0\0aaaaaaaceeeeiiii\0nooooo\0ouuuuy\0y"sv;
                    const auto next = static_cast<unsigned char>(text[i + 1]);
                    const std::size_t index = next - 0x80u;
                    if (next >= 0x80 && index < Base.size() && Base[index] != '\0') {
                        folded += Base[index];
                        ++i;
                    } else {
                        folded += text[i];
                    }
                } else {
                    folded += text[i];
                }
            }
            return folded;
        }

        private:
        std::string m_folded;
    };
}
