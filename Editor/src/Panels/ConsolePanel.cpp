#include "Panels/ConsolePanel.hpp"

#include "Search.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/UI/Context.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace FluxEditor {
    using namespace Flux;

    namespace {
        constexpr std::size_t Index(LogLevel level) {
            return static_cast<std::size_t>(level);
        }

        UI::Icon LevelIcon(LogLevel level) {
            switch (level) {
                case LogLevel::Warning:
                    return UI::Icon::Warning;
                case LogLevel::Error:
                    return UI::Icon::Error;
                case LogLevel::Info:
                    break;
            }
            return UI::Icon::Info;
        }

        // Première ligne d'un message, raccourcie (à la limite d'un caractère UTF-8) : une ligne de liste ou de barre.
        std::string Summary(std::string_view text, std::size_t maxBytes) {
            text = text.substr(0, text.find('\n'));
            if (text.size() <= maxBytes) {
                return std::string(text);
            }
            std::size_t cut = maxBytes;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
                --cut;
            }
            return std::string(text.substr(0, cut)) + "...";
        }

        // Texte raccourci (avec « ... ») pour tenir dans width pixels ; coupé entre deux caractères UTF-8.
        std::string FitText(const UI::Context& ui, std::string_view text, float width) {
            if (ui.MeasureText(text).x <= width) {
                return std::string(text);
            }
            // Recherche par dichotomie de la plus longue coupe qui tient : quelques mesures au lieu d'une par caractère.
            std::size_t low = 0;
            std::size_t high = text.size();
            while (low < high) {
                const std::size_t middle = (low + high + 1) / 2;
                if (ui.MeasureText(std::string(text.substr(0, middle)) + "...").x <= width) {
                    low = middle;
                } else {
                    high = middle - 1;
                }
            }
            while (low > 0 && (static_cast<unsigned char>(text[low]) & 0xC0) == 0x80) {
                --low;
            }
            return std::string(text.substr(0, low)) + "...";
        }

        // Largeur d'un MenuBarButton ou d'un MenuBarText (pour aligner des éléments à droite de la barre).
        float BarItemWidth(const UI::Context& ui, std::string_view text, bool icon) {
            const UI::Style& style = ui.GetStyle();
            const float iconSize = icon ? std::round(style.fontSize * 0.9f) : 0.0f;
            const float gap = icon && !text.empty() ? style.framePadding.x * 0.75f : 0.0f;
            return iconSize + gap + (text.empty() ? 0.0f : ui.MeasureText(text).x) + 2.0f * style.framePadding.x;
        }
    }

    void ConsolePanel::Read() {
        const std::size_t before = m_entries.size();
        m_lastId = Log::Read(m_lastId, m_entries);
        if (m_entries.size() == before) {
            return;
        }
        for (std::size_t i = before; i < m_entries.size(); ++i) {
            ++m_counts[Index(m_entries[i].level)];
        }
        if (m_entries.size() > Log::MaxEntries) {
            const auto drop = static_cast<std::ptrdiff_t>(m_entries.size() - Log::MaxEntries);
            for (auto it = m_entries.begin(); it != m_entries.begin() + drop; ++it) {
                --m_counts[Index(it->level)];
            }
            m_entries.erase(m_entries.begin(), m_entries.begin() + drop);
        }
        m_scrollToEnd = true;
    }

    void ConsolePanel::Open(Engine& engine, LogLevel level) {
        engine.GetUi().FocusPanel("Console");
        m_shown[Index(level)] = true; // le message cliqué doit être dans la liste
        m_search.clear();
        m_scrollToEnd = true;
    }

    void ConsolePanel::Update(Engine& engine) {
        Read();
        UI::Context& ui = engine.GetUi();
        if (!m_placed) {
            ui.DockPanelNextTo("Console", "Contenu"); // disposition enregistrée avant la console : en onglet en bas
            m_placed = true;
        }
        const float screenHeight = static_cast<float>(engine.GetWindow().GetSize().y);
        ui.SetNextPanelIcon(UI::Icon::Console);
        if (ui.BeginPanel("Console", {8.0f, std::max(8.0f, screenHeight - 268.0f)}, 560.0f, 240.0f)) {
            if (ui.Button("Effacer")) {
                m_entries.clear();
                m_counts = {};
                m_selected = 0;
            }
            ui.SetItemTooltip("Vider la console");
            constexpr struct
            {
                LogLevel level;
                std::string_view id;
                std::string_view tooltip;
            } Filters[] = {{LogLevel::Info, "infos", "Afficher les informations"},
                           {LogLevel::Warning, "avertissements", "Afficher les avertissements"},
                           {LogLevel::Error, "erreurs", "Afficher les erreurs"}};
            for (const auto& filter : Filters) {
                ui.SameLine();
                ui.ToggleButton(std::format("{}##{}", m_counts[Index(filter.level)], filter.id), LevelIcon(filter.level),
                                m_shown[Index(filter.level)]);
                ui.SetItemTooltip(filter.tooltip);
            }
            ui.SameLine();
            ui.SearchField("recherche", m_search, "Filtrer les messages...");

            ui.BeginChild("messages", 0.0f, 0.0f);
            const SearchQuery query(m_search);
            bool any = false;
            for (const LogEntry& entry : m_entries) {
                if (!m_shown[Index(entry.level)] || !query.Matches(entry.text)) {
                    continue;
                }
                any = true;
                ui.PushId(entry.id);
                const std::string line = entry.time + "   " + Summary(entry.text, 300);
                if (ui.TreeNode(line, entry.id == m_selected, true, false, LevelIcon(entry.level)).clicked) {
                    m_selected = entry.id;
                }
                if (entry.text.size() > 300 || entry.text.find('\n') != std::string::npos) {
                    ui.SetItemTooltip(entry.text); // message de plusieurs lignes, ou raccourci : en entier
                }
                ui.PopId();
            }
            if (!any) {
                ui.TextDisabled(m_entries.empty() ? "Aucun message." : "Aucun message ne correspond aux filtres.");
            }
            if (std::exchange(m_scrollToEnd, false)) {
                ui.SetScrollToEnd();
            }
            ui.EndChild();
        }
        ui.EndPanel();
    }

    void ConsolePanel::DrawStatusBar(Engine& engine, std::string_view details) {
        Read();
        UI::Context& ui = engine.GetUi();
        if (!ui.BeginStatusBar()) {
            return;
        }
        const UI::Style& style = ui.GetStyle();
        const float screenWidth = static_cast<float>(engine.GetWindow().GetSize().x);

        // À droite : avertissements et erreurs (s'il y en a), puis les détails de la scène.
        const std::string warnings = std::to_string(m_counts[Index(LogLevel::Warning)]);
        const std::string errors = std::to_string(m_counts[Index(LogLevel::Error)]);
        const bool showWarnings = m_counts[Index(LogLevel::Warning)] > 0;
        const bool showErrors = m_counts[Index(LogLevel::Error)] > 0;
        float rightWidth = BarItemWidth(ui, details, false) + style.panelPadding * 0.5f;
        if (showWarnings) {
            rightWidth += BarItemWidth(ui, warnings, true) + style.itemSpacing;
        }
        if (showErrors) {
            rightWidth += BarItemWidth(ui, errors, true) + style.itemSpacing;
        }
        const float rightX = std::round(screenWidth - rightWidth);

        // À gauche : le dernier message, raccourci pour ne pas déborder sur la droite.
        if (m_entries.empty()) {
            ui.MenuBarText("Prêt", UI::Icon::None, true);
        } else {
            const LogEntry& last = m_entries.back();
            const float room = rightX - 4.0f * style.framePadding.x - style.fontSize;
            const std::string text = FitText(ui, Summary(last.text, 300), room);
            if (ui.MenuBarButton(text + "##dernier", false, LevelIcon(last.level))) {
                Open(engine, last.level);
            }
            ui.SetItemTooltip("Ouvrir la console");
        }

        ui.SetMenuBarCursor(rightX);
        if (showWarnings) {
            if (ui.MenuBarButton(warnings + "##avertissements", false, UI::Icon::Warning)) {
                Open(engine, LogLevel::Warning);
            }
            ui.SetItemTooltip("Avertissements : ouvrir la console");
        }
        if (showErrors) {
            if (ui.MenuBarButton(errors + "##erreurs", false, UI::Icon::Error)) {
                Open(engine, LogLevel::Error);
            }
            ui.SetItemTooltip("Erreurs : ouvrir la console");
        }
        ui.MenuBarText(details, UI::Icon::None, true);
        ui.EndStatusBar();
    }
}
