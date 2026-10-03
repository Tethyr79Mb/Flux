#pragma once

#include "Flux/Export.hpp"

#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace Flux {
    enum class LogLevel : std::uint8_t { Info, Warning, Error };

    struct LogEntry
    {
        std::uint64_t id = 0; // croissant à partir de 1 : Log::Read reprend après le dernier lu
        LogLevel level = LogLevel::Info;
        std::string time;     // heure locale, « 14:05:32 »
        std::string text;     // UTF-8, une ou plusieurs lignes
    };

    // Journal du moteur, de l'éditeur et du jeu : chaque message est écrit dans la console du programme (stdout pour
    // les informations, stderr sinon) et gardé en mémoire (les plus récents seulement) pour le panneau Console de
    // l'éditeur. Utilisable depuis n'importe quel thread.
    //     Log::Warning("modèle {} : {}", name, error);
    namespace Log {
        // Nombre de messages gardés en mémoire : les plus anciens sont oubliés au-delà.
        inline constexpr std::size_t MaxEntries = 2000;

        FLUX_API void Write(LogLevel level, std::string text);
        // Ajoute à out les messages gardés dont l'id est supérieur à after ; retourne l'id du dernier message écrit
        // (after s'il n'y en a pas de nouveau).
        FLUX_API std::uint64_t Read(std::uint64_t after, std::vector<LogEntry>& out);

        template <class... Args>
        void Info(std::format_string<Args...> format, Args&&... args) {
            Write(LogLevel::Info, std::format(format, std::forward<Args>(args)...));
        }
        template <class... Args>
        void Warning(std::format_string<Args...> format, Args&&... args) {
            Write(LogLevel::Warning, std::format(format, std::forward<Args>(args)...));
        }
        template <class... Args>
        void Error(std::format_string<Args...> format, Args&&... args) {
            Write(LogLevel::Error, std::format(format, std::forward<Args>(args)...));
        }
    }
}
