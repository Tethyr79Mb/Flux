#include "Panels/SettingsWindow.hpp"

#include <Flux/Audio/AudioSystem.hpp>
#include <Flux/Core/Engine.hpp>
#include <Flux/Core/Log.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Renderer/Renderer.hpp>
#include <Flux/UI/Context.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace FluxEditor {
    using namespace Flux;
    using Json = nlohmann::json;

    void SettingsWindow::Load(Engine& engine) {
        std::ifstream file(FilePath(), std::ios::binary);
        if (!file.is_open()) {
            return; // premier lancement : thème par défaut
        }
        const Json json = Json::parse(file, nullptr, false);
        if (json.is_discarded() || !json.is_object()) {
            return;
        }
        if (const auto theme = json.find("theme"); theme != json.end() && theme->is_string()) {
            const UI::Theme found = UI::ThemeFromName(theme->get<std::string>());
            if (found != UI::Theme::Count) {
                m_theme = found;
                engine.GetUi().SetStyle(UI::Style::FromTheme(m_theme));
            }
        }
        if (const auto volume = json.find("volume"); volume != json.end() && volume->is_number()) {
            m_volume = std::clamp(volume->get<float>(), 0.0f, 1.0f);
            engine.GetAudio().SetMasterVolume(m_volume);
        }
    }

    void SettingsWindow::Save() const {
        const Json json{{"format", "flux.editor-settings"},
                        {"version", 1},
                        {"theme", UI::GetThemeNames()[static_cast<std::size_t>(m_theme)]},
                        {"volume", m_volume}};
        std::ofstream file(FilePath(), std::ios::binary | std::ios::trunc);
        file << json.dump(2);
        if (!file) {
            Log::Warning("paramètres de l'éditeur non enregistrés");
        }
    }

    std::filesystem::path SettingsWindow::FilePath() {
        return Platform::GetExecutableDirectory() / "FluxEditor.settings.json";
    }

    void SettingsWindow::Draw(Engine& engine) {
        if (!m_open) {
            return;
        }
        UI::Context& ui = engine.GetUi();
        const Math::Vector2i screen = engine.GetWindow().GetSize();
        const float width = 420.0f;
        // Au milieu de l'écran à la première ouverture ; ensuite là où on l'a laissée.
        if (ui.BeginPanel("Paramètres", {(static_cast<float>(screen.x) - width) * 0.5f, static_cast<float>(screen.y) * 0.25f},
                          width, 0.0f, &m_open)) {
            if (ui.Tab("Graphiques", m_tab == Tab::Graphics)) {
                m_tab = Tab::Graphics;
            }
            ui.SameLine();
            if (ui.Tab("Son", m_tab == Tab::Sound)) {
                m_tab = Tab::Sound;
            }
            ui.SameLine();
            if (ui.Tab("Interface", m_tab == Tab::Interface)) {
                m_tab = Tab::Interface;
            }
            ui.Separator();

            if (m_tab == Tab::Graphics) {
                Renderer& renderer = engine.GetRenderer();
                ui.LabelText("GPU", renderer.GetDeviceName());
                bool vsync = renderer.IsVSyncEnabled();
                if (ui.Checkbox("V-Sync", vsync)) {
                    renderer.SetVSync(vsync);
                }
                ui.LabelText("Présentation", renderer.GetPresentModeName());
            } else if (m_tab == Tab::Sound) {
                AudioSystem& audio = engine.GetAudio();
                float percent = m_volume * 100.0f;
                if (ui.DragFloat("Volume général (%)", percent, 0.5f, 0.0, 100.0, 0)) {
                    m_volume = percent / 100.0f;
                    audio.SetMasterVolume(m_volume);
                    m_volumeChanged = true;
                }
                // Enregistré au lâcher, pas à chaque pixel glissé.
                if (m_volumeChanged && !ui.IsItemActive()) {
                    m_volumeChanged = false;
                    Save();
                }
                if (!audio.IsAvailable()) {
                    ui.TextDisabled("Aucune sortie son : le jeu reste muet.");
                }
                ui.TextDisabled("Les sons (composant Son) jouent en mode Jouer.");
            } else {
                int theme = static_cast<int>(m_theme);
                if (ui.Choice("Thème", theme, UI::GetThemeNames())) {
                    m_theme = static_cast<UI::Theme>(theme);
                    ui.SetStyle(UI::Style::FromTheme(m_theme)); // visible dès la frame suivante
                    Save();
                }
            }
        }
        ui.EndPanel();
    }
}
