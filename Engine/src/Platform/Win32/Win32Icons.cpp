#include "Flux/Platform/Icons.hpp"

#include "Platform/Win32/Win32Utf.hpp"

#include <commctrl.h> // avant commoncontrols.h : IMAGEINFO, ILD_*
#include <commoncontrols.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace Flux::Platform {
    namespace {
        // IID_IImageList, écrit ici plutôt que pris dans uuid.lib (absent de certaines versions de MinGW).
        constexpr GUID ImageListId = {0x46eb5926, 0x582e, 0x4017, {0x9f, 0xdf, 0xe8, 0x99, 0x8d, 0xaa, 0x09, 0x50}};

        // Pixels d'un bitmap en BGRA 32 bits, lignes de haut en bas.
        std::vector<std::uint8_t> ReadBitmap(HBITMAP bitmap, int width, int height) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -height; // négatif : de haut en bas
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
            HDC dc = GetDC(nullptr);
            const int lines = GetDIBits(dc, bitmap, 0, static_cast<UINT>(height), pixels.data(), &info, DIB_RGB_COLORS);
            ReleaseDC(nullptr, dc);
            if (lines != height) {
                pixels.clear();
            }
            return pixels;
        }

        std::optional<ImageData> ToImage(HICON icon) {
            ICONINFO parts{};
            if (!GetIconInfo(icon, &parts)) {
                return std::nullopt;
            }
            std::optional<ImageData> image;
            BITMAP color{};
            if (parts.hbmColor && GetObjectW(parts.hbmColor, sizeof(color), &color) && color.bmWidth > 0 && color.bmHeight > 0) {
                const int width = color.bmWidth;
                const int height = color.bmHeight;
                std::vector<std::uint8_t> bgra = ReadBitmap(parts.hbmColor, width, height);
                if (!bgra.empty()) {
                    bool hasAlpha = false;
                    for (std::size_t i = 3; i < bgra.size(); i += 4) {
                        hasAlpha |= bgra[i] != 0;
                    }
                    // Vieilles icônes sans alpha : le masque dit ce qui est transparent (blanc = transparent).
                    std::vector<std::uint8_t> mask;
                    if (!hasAlpha && parts.hbmMask) {
                        mask = ReadBitmap(parts.hbmMask, width, height);
                    }
                    image.emplace();
                    image->size = {width, height};
                    image->rgba.resize(bgra.size());
                    for (std::size_t i = 0; i < bgra.size(); i += 4) {
                        image->rgba[i + 0] = bgra[i + 2];
                        image->rgba[i + 1] = bgra[i + 1];
                        image->rgba[i + 2] = bgra[i + 0];
                        image->rgba[i + 3] = hasAlpha ? bgra[i + 3] : mask.empty() || mask[i] == 0 ? 255 : 0;
                    }
                    // Selon la version de Windows, les couleurs viennent prémultipliées par l'alpha ou non : aucune
                    // composante au-dessus de l'alpha, et au moins un pixel semi-transparent coloré, c'est prémultiplié.
                    // L'overlay attend des couleurs droites.
                    bool premultiplied = hasAlpha;
                    bool anyPartial = false;
                    for (std::size_t i = 0; i < image->rgba.size() && premultiplied; i += 4) {
                        const std::uint8_t alpha = image->rgba[i + 3];
                        premultiplied = image->rgba[i] <= alpha && image->rgba[i + 1] <= alpha && image->rgba[i + 2] <= alpha;
                        anyPartial |= alpha > 0 && alpha < 255;
                    }
                    if (premultiplied && anyPartial) {
                        for (std::size_t i = 0; i < image->rgba.size(); i += 4) {
                            const int alpha = image->rgba[i + 3];
                            for (std::size_t c = 0; c < 3 && alpha > 0; ++c) {
                                image->rgba[i + c] = static_cast<std::uint8_t>(std::min(255, image->rgba[i + c] * 255 / alpha));
                            }
                        }
                    }
                }
            }
            if (parts.hbmColor) {
                DeleteObject(parts.hbmColor);
            }
            if (parts.hbmMask) {
                DeleteObject(parts.hbmMask);
            }
            return image;
        }
    }

    std::optional<ImageData> GetShellIcon(std::string_view extension) {
        // Les fonctions du shell demandent COM ; déjà initialisé (autre mode) : on s'en sert tel quel.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const bool folder = extension.empty();
        // SHGFI_USEFILEATTRIBUTES : pas d'accès au disque, le nom sert seulement à trouver le type.
        const std::wstring name = folder ? std::wstring(L"dossier") : L"fichier" + Win32::ToWide(extension);
        SHFILEINFOW info{};
        std::optional<ImageData> image;
        if (SHGetFileInfoW(name.c_str(), folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                           SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX)) {
            IImageList* list = nullptr;
            if (SUCCEEDED(SHGetImageList(SHIL_EXTRALARGE, ImageListId, reinterpret_cast<void**>(&list))) && list) {
                HICON icon = nullptr;
                if (SUCCEEDED(list->GetIcon(info.iIcon, ILD_TRANSPARENT, &icon)) && icon) {
                    image = ToImage(icon);
                    DestroyIcon(icon);
                }
                list->Release();
            }
        }
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
        return image;
    }
}
