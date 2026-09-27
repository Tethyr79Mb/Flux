#include "Flux/Platform/Dialogs.hpp"

#include "Flux/Platform/Window.hpp"
#include "Platform/Win32/Win32Utf.hpp"

#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

namespace Flux::Platform {
    namespace {
        HWND OwnerHandle(const Window& owner) {
            return static_cast<HWND>(owner.GetNativeHandle());
        }

        // "Scènes Flux (*.scene.json)\0*.scene.json\0Tous les fichiers\0*.*\0\0" : chaînes séparées par des zéros.
        // Plusieurs extensions (".png;.jpg") donnent "*.png;*.jpg", la syntaxe de la boîte de dialogue.
        std::wstring MakeFilter(const FileType& type) {
            std::wstring pattern;
            std::string_view extensions = type.extension;
            while (!extensions.empty()) {
                const std::size_t end = extensions.find(';');
                if (!pattern.empty()) {
                    pattern += L';';
                }
                pattern += L"*" + Win32::ToWide(extensions.substr(0, end));
                extensions = end == std::string_view::npos ? std::string_view{} : extensions.substr(end + 1);
            }
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

    bool AskYesNo(const Window& owner, std::string_view title, std::string_view question) {
        return MessageBoxW(OwnerHandle(owner), Win32::ToWide(question).c_str(), Win32::ToWide(title).c_str(),
                           MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
    }

    std::expected<void, std::string> MoveToRecycleBin(const std::filesystem::path& path) {
        // SHFileOperation attend une liste de chemins terminée par deux zéros.
        std::wstring from = std::filesystem::absolute(path).wstring();
        from.push_back(L'\0');
        SHFILEOPSTRUCTW operation{};
        operation.wFunc = FO_DELETE;
        operation.pFrom = from.c_str();
        operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        const int result = SHFileOperationW(&operation);
        if (result != 0 || operation.fAnyOperationsAborted) {
            return std::unexpected("impossible de supprimer « " + Win32::ToUtf8(path.filename().wstring()) + " » (code "
                                   + std::to_string(result) + ")");
        }
        return {};
    }

    void ShowInExplorer(const std::filesystem::path& path) {
        const std::wstring arguments = L"/select,\"" + std::filesystem::absolute(path).wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
    }

    void ShowError(const Window& owner, std::string_view title, std::string_view message) {
        MessageBoxW(OwnerHandle(owner), Win32::ToWide(message).c_str(), Win32::ToWide(title).c_str(), MB_OK | MB_ICONERROR);
    }

    std::optional<Math::LinearColor> ShowColorDialog(const Window& owner, const Math::LinearColor& initial) {
        // Les 16 cases « couleurs personnalisées » : partagées par tous les appels, le temps de la session.
        static COLORREF customColors[16] = {RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
                                            RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
                                            RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
                                            RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255)};
        // Le nuancier travaille en sRGB 8 bits, comme les champs de l'inspecteur.
        const auto toByte = [](float linear) {
            return static_cast<BYTE>(std::lround(Math::LinearColor::LinearToSrgb(std::clamp(linear, 0.0f, 1.0f)) * 255.0f));
        };
        CHOOSECOLORW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = OwnerHandle(owner);
        dialog.rgbResult = RGB(toByte(initial.r), toByte(initial.g), toByte(initial.b));
        dialog.lpCustColors = customColors;
        dialog.Flags = CC_RGBINIT | CC_FULLOPEN | CC_ANYCOLOR;
        if (!ChooseColorW(&dialog)) {
            return std::nullopt;
        }
        Math::LinearColor color = Math::LinearColor::FromSrgb8(GetRValue(dialog.rgbResult), GetGValue(dialog.rgbResult),
                                                               GetBValue(dialog.rgbResult));
        color.a = initial.a;
        return color;
    }
}
