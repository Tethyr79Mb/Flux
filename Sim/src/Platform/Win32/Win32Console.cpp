#include "Flux/Platform/Console.hpp"

#include "Platform/Win32/Win32Utf.hpp"

#include <atomic>

#include <windows.h>
#include <shellapi.h>

namespace Flux::Platform {
    namespace {
        std::atomic<bool> closeRequested = false;
        HANDLE closeHandled = nullptr; // événement : le programme a fini de ranger

        // Appelé par Windows sur un autre fil. Pour une fermeture, le processus est tué dès le retour (et de toute
        // façon après 5 s) : on attend que la boucle principale ait fini.
        BOOL WINAPI OnConsoleEvent(DWORD event) {
            switch (event) {
                case CTRL_C_EVENT:
                case CTRL_BREAK_EVENT:
                case CTRL_CLOSE_EVENT:
                    closeRequested = true;
                    WaitForSingleObject(closeHandled, 4000);
                    return TRUE;
                default:
                    return FALSE; // fin de session : traitement par défaut
            }
        }
    }

    void UseUtf8Console() {
        SetConsoleOutputCP(CP_UTF8);
    }

    void CatchConsoleClose() {
        if (!closeHandled) {
            closeHandled = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            SetConsoleCtrlHandler(OnConsoleEvent, TRUE);
        }
    }

    bool ConsoleCloseRequested() {
        return closeRequested;
    }

    void ConsoleCloseHandled() {
        if (closeHandled) {
            SetEvent(closeHandled);
        }
    }

    std::vector<std::string> GetCommandLineArguments() {
        int count = 0;
        LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        std::vector<std::string> result;
        for (int i = 1; arguments != nullptr && i < count; ++i) {
            result.push_back(Win32::ToUtf8(arguments[i]));
        }
        LocalFree(arguments);
        return result;
    }
}
