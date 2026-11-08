#include "Flux/Platform/Console.hpp"

#include <atomic>
#include <csignal>
#include <fstream>
#include <iterator>

namespace Flux::Platform {
    namespace {
        // Écrit depuis le gestionnaire de signal : un atomique sans verrou y est permis.
        std::atomic<bool> closeRequested = false;
        static_assert(std::atomic<bool>::is_always_lock_free);

        void OnSignal(int) {
            closeRequested = true;
        }
    }

    // Les terminaux Linux sont déjà en UTF-8.
    void UseUtf8Console() {}

    void CatchConsoleClose() {
        struct sigaction action{};
        action.sa_handler = OnSignal;
        sigemptyset(&action.sa_mask);
        for (const int signal : {SIGINT, SIGTERM, SIGHUP}) {
            sigaction(signal, &action, nullptr);
        }
    }

    bool ConsoleCloseRequested() {
        return closeRequested;
    }

    // Rien n'attend : le processus finit quand la boucle a rangé.
    void ConsoleCloseHandled() {}

    std::vector<std::string> GetCommandLineArguments() {
        // Arguments séparés par des zéros, le premier est le programme.
        std::ifstream file("/proc/self/cmdline", std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<std::string> result;
        std::size_t start = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\0') {
                result.emplace_back(text, start, i - start);
                start = i + 1;
            }
        }
        if (!result.empty()) {
            result.erase(result.begin());
        }
        return result;
    }
}
