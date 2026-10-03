#include "Flux/Core/Log.hpp"

#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>

namespace Flux::Log {
    namespace {
        struct Journal
        {
            std::mutex mutex;
            std::deque<LogEntry> entries;
            std::uint64_t lastId = 0;
        };

        Journal& GetJournal() {
            static Journal journal;
            return journal;
        }

        std::string LocalTime() {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            char text[16];
            std::strftime(text, sizeof(text), "%H:%M:%S", &local);
            return text;
        }
    }

    void Write(LogLevel level, std::string text) {
        std::FILE* stream = level == LogLevel::Info ? stdout : stderr;
        std::fprintf(stream, "%s%s\n", level == LogLevel::Error ? "Erreur : " : level == LogLevel::Warning ? "Attention : " : "",
                     text.c_str());
        std::fflush(stream);

        Journal& journal = GetJournal();
        std::string time = LocalTime();
        const std::scoped_lock lock(journal.mutex);
        journal.entries.push_back({++journal.lastId, level, std::move(time), std::move(text)});
        if (journal.entries.size() > MaxEntries) {
            journal.entries.pop_front();
        }
    }

    std::uint64_t Read(std::uint64_t after, std::vector<LogEntry>& out) {
        Journal& journal = GetJournal();
        const std::scoped_lock lock(journal.mutex);
        if (journal.lastId == after) {
            return after; // le cas de presque toutes les frames : rien de nouveau, rien à parcourir
        }
        // Les ids se suivent : le premier nouveau message est à un rang connu.
        const std::uint64_t first = journal.entries.empty() ? journal.lastId + 1 : journal.entries.front().id;
        const std::size_t start = after < first ? 0 : static_cast<std::size_t>(after - first + 1);
        for (std::size_t i = start; i < journal.entries.size(); ++i) {
            out.push_back(journal.entries[i]);
        }
        return journal.lastId;
    }
}
