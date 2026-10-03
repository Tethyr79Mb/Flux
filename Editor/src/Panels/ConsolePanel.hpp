#pragma once

#include <Flux/Core/Log.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Flux {
    class Engine;
}

namespace FluxEditor {
    // Panneau « Console » : les messages du journal (Flux::Log) du moteur, de l'éditeur et du jeu, filtrés par niveau
    // et par texte, et la barre d'état en bas de l'éditeur, qui montre le dernier message (un clic ouvre la console).
    class ConsolePanel {
        public:
        // Lit les nouveaux messages et décrit le panneau ; à chaque frame, même quand l'onglet est caché.
        void Update(Flux::Engine& engine);
        // Barre d'état : le dernier message à gauche, le nombre d'avertissements et d'erreurs puis details à droite.
        // Avant la disposition des panneaux (EditorLayout::Update), qui laisse sa place en bas de l'écran.
        void DrawStatusBar(Flux::Engine& engine, std::string_view details);

        private:
        void Read();
        void Open(Flux::Engine& engine, Flux::LogLevel level);

        std::vector<Flux::LogEntry> m_entries; // au plus Log::MaxEntries, du plus ancien au plus récent
        std::uint64_t m_lastId = 0;
        std::array<std::size_t, 3> m_counts{};  // par niveau (LogLevel), dans m_entries
        std::array<bool, 3> m_shown{true, true, true};
        std::string m_search;
        std::uint64_t m_selected = 0;
        bool m_scrollToEnd = true; // nouveaux messages : la liste descend jusqu'au dernier
        bool m_placed = false;
    };
}
