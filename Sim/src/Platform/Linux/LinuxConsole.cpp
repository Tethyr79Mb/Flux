#include "Flux/Platform/Console.hpp"

#include <atomic>
#include <csignal>

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
}
