#include "Flux/Platform/Dialogs.hpp"

#include "Flux/Platform/Window.hpp"
#include "Platform/Win32/Win32Utf.hpp"

#include <commdlg.h>

#include <algorithm>
#include <cwctype>
#include <vector>

namespace Flux::Platform {
    namespace {
        HWND OwnerHandle(const Window& owner) {
            return static_cast<HWND>(owner.GetNativeHandle());
        }

        // "Scènes Flux (*.scene.json)\0*.scene.json\0Tous les fichiers\0*.*\0\0" : chaînes séparées par des zéros.
        std::wstring MakeFilter(const FileType& type) {
            const std::wstring pattern = L"*" + Win32::ToWide(type.extension);
            std::wstring filter = Win32::ToWide(type.description) + L" (" + pattern + L")";
            filter += L'\0';
            filter += pattern;
            filter += L'\0';
            filter += L"Tous les fichiers";
            filter += L'\0';
            filter += L"*.*";
            filter += L'\0';
            return filter; // le dernier \0 est celui de la chaîne
        }

        bool EndsWith(const std::wstring& text, const std::wstring& suffix) {
            if (suffix.size() > text.size()) {
                return false;
            }
            return std::equal(suffix.rbegin(), suffix.rend(), text.rbegin(), [](wchar_t a, wchar_t b) {
                return std::towlower(a) == std::towlower(b);
            });
        }

        std::optional<std::filesystem::path> Show(const Window& owner, std::string_view title, const FileType& type,
                                                  const std::filesystem::path& initialDirectory, std::string_view initialName,
                                                  bool save) {
            const std::wstring filter = MakeFilter(type);
            const std::wstring wideTitle = Win32::ToWide(title);
            const std::wstring directory = initialDirectory.wstring();
            std::vector<wchar_t> buffer(32768, L'\0'); // chemins longs
            const std::wstring name = Win32::ToWide(initialName);
            std::copy_n(name.begin(), std::min(name.size(), buffer.size() - 1), buffer.begin());

            OPENFILENAMEW dialog{};
            dialog.lStructSize = sizeof(dialog);
            dialog.hwndOwner = OwnerHandle(owner);
            dialog.lpstrFilter = filter.c_str();
            dialog.nFilterIndex = 1;
            dialog.lpstrFile = buffer.data();
            dialog.nMaxFile = static_cast<DWORD>(buffer.size());
            dialog.lpstrInitialDir = directory.empty() ? nullptr : directory.c_str();
            dialog.lpstrTitle = wideTitle.c_str();
            // NOCHANGEDIR : le dossier courant du programme ne suit pas la navigation dans la boîte de dialogue.
            dialog.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST
                         | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);

            const BOOL chosen = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
            if (!chosen) {
                return std::nullopt; // annulé (ou erreur : CommDlgExtendedError, rien d'utile à montrer)
            }
            std::wstring path(buffer.data());
            if (save && dialog.nFilterIndex == 1) {
                // Extension double (".scene.json") : la boîte ne sait ajouter que la dernière, on s'en charge.
                const std::wstring extension = Win32::ToWide(type.extension);
                if (!EndsWith(path, extension)) {
                    path += extension;
                    // La boîte n'a pas pu signaler ce fichier-là comme existant.
                    if (std::filesystem::exists(std::filesystem::path(path))) {
                        const std::wstring question = L"« " + std::filesystem::path(path).filename().wstring()
                                                    + L" » existe déjà.\nVoulez-vous le remplacer ?";
                        if (MessageBoxW(OwnerHandle(owner), question.c_str(), wideTitle.c_str(),
                                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                            return std::nullopt;
                        }
                    }
                }
            }
            return std::filesystem::path(path);
        }
    }

    std::optional<std::filesystem::path> ShowOpenFileDialog(const Window& owner, std::string_view title, const FileType& type,
                                                            const std::filesystem::path& initialDirectory) {
        return Show(owner, title, type, initialDirectory, {}, false);
    }

    std::optional<std::filesystem::path> ShowSaveFileDialog(const Window& owner, std::string_view title, const FileType& type,
                                                            const std::filesystem::path& initialDirectory,
                                                            std::string_view initialName) {
        return Show(owner, title, type, initialDirectory, initialName, true);
    }

    Answer AskYesNoCancel(const Window& owner, std::string_view title, std::string_view question) {
        const int answer = MessageBoxW(OwnerHandle(owner), Win32::ToWide(question).c_str(), Win32::ToWide(title).c_str(),
                                       MB_YESNOCANCEL | MB_ICONWARNING);
        return answer == IDYES ? Answer::Yes : answer == IDNO ? Answer::No : Answer::Cancel;
    }

    void ShowError(const Window& owner, std::string_view title, std::string_view message) {
        MessageBoxW(OwnerHandle(owner), Win32::ToWide(message).c_str(), Win32::ToWide(title).c_str(), MB_OK | MB_ICONERROR);
    }
}
