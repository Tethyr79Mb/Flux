#include "Panels/ViewCapture.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Core/Log.hpp>
#include <Flux/Core/Project.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Input.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Renderer/Renderer.hpp>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace FluxEditor {
    using namespace Flux;

    namespace {
        // L'image arrive deux ou trois frames après la demande ; au-delà, elle ne viendra plus.
        constexpr int MaxWaitFrames = 30;

        // Date et heure locales, triables et permises dans un nom de fichier : « 2026-11-16 14-05-32 ».
        std::string LocalStamp() {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#ifdef _WIN32
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            char text[32];
            std::strftime(text, sizeof(text), "%Y-%m-%d %H-%M-%S", &local);
            return text;
        }
    }

    ImageData CropImage(const ImageData& source, int left, int top, int right, int bottom) {
        left = std::clamp(left, 0, source.size.x);
        right = std::clamp(right, left, source.size.x);
        top = std::clamp(top, 0, source.size.y);
        bottom = std::clamp(bottom, top, source.size.y);
        if (right - left <= 0 || bottom - top <= 0) {
            return source;
        }
        ImageData target;
        target.size = {right - left, bottom - top};
        target.rgba.resize(static_cast<std::size_t>(target.size.x) * static_cast<std::size_t>(target.size.y) * 4);
        for (int y = 0; y < target.size.y; ++y) {
            const std::uint8_t* from = source.rgba.data() + (static_cast<std::size_t>(top + y) * source.size.x + left) * 4;
            std::copy_n(from, static_cast<std::size_t>(target.size.x) * 4,
                        target.rgba.data() + static_cast<std::size_t>(y) * target.size.x * 4);
        }
        return target;
    }

    void ViewCapture::HandleShortcuts(Engine& engine) {
        m_frameRequested = false;
        if (m_pending || !engine.GetWindow().GetInput().WasKeyPressed(Key::F12) || engine.GetUi().WantsKeyboard()) {
            return;
        }
        if (!engine.GetRenderer().RequestCapture()) {
            Log::Warning("capture impossible : la fenêtre ne permet pas la copie de son image (pilote)");
            return;
        }
        m_frameRequested = true;
        m_pending = true;
        m_waitedFrames = 0;
    }

    std::filesystem::path ViewCapture::Directory(const Engine& engine) {
        // Sans projet : à côté de FluxEditor.exe.
        const Project* project = engine.GetProject();
        return (project ? project->file.parent_path() : Platform::GetExecutableDirectory()) / "Captures";
    }

    void ViewCapture::Update(Engine& engine, const UI::Rect& view) {
        if (m_frameRequested) {
            m_view = view;
        }
        if (!m_pending) {
            return;
        }
        // Le serveur MCP relit la même capture du moteur : si elle part chez lui, F12 ne reste pas bloquée.
        std::optional<ImageData> image = engine.GetRenderer().TakeCapture();
        if (!image) {
            if (++m_waitedFrames > MaxWaitFrames) {
                m_pending = false;
                Log::Warning("capture perdue (demandée en même temps par Claude ?) : appuyer de nouveau sur F12");
            }
            return;
        }
        m_pending = false;
        ImageData cropped = CropImage(*image, static_cast<int>(std::lround(m_view.min.x)), static_cast<int>(std::lround(m_view.min.y)),
                                      static_cast<int>(std::lround(m_view.max.x)), static_cast<int>(std::lround(m_view.max.y)));
        for (std::size_t i = 3; i < cropped.rgba.size(); i += 4) {
            cropped.rgba[i] = 255; // l'alpha de la fenêtre n'a pas de sens : opaque, sinon les visionneuses montrent un damier
        }
        const std::filesystem::path directory = Directory(engine);
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        const std::string stamp = LocalStamp();
        std::filesystem::path path = directory / Platform::PathFromUtf8("Capture " + stamp + ".png");
        for (int i = 2; std::filesystem::exists(path, error); ++i) { // deux captures dans la même seconde
            path = directory / Platform::PathFromUtf8(std::format("Capture {} ({}).png", stamp, i));
        }
        const std::vector<std::uint8_t> png = EncodePng(cropped);
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        if (!file) {
            Log::Error("capture non enregistrée : {}", Platform::ToUtf8(path));
            return;
        }
        Log::Info("capture enregistrée ({} x {}) : {}", cropped.size.x, cropped.size.y, Platform::ToUtf8(path));
    }
}
