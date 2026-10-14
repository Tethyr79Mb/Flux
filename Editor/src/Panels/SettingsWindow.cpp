#include "Panels/SettingsWindow.hpp"

#include <Flux/Audio/AudioSystem.hpp>
#include <Flux/Core/Engine.hpp>
#include <Flux/Core/Log.hpp>
#include <Flux/Input/InputActions.hpp>
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
        if (const auto bloom = json.find("bloom"); bloom != json.end() && bloom->is_number()) {
            m_bloom = std::clamp(bloom->get<float>(), 0.0f, 1.0f);
        }
        if (const auto enabled = json.find("bloomEnabled"); enabled != json.end() && enabled->is_boolean()) {
            m_bloomEnabled = enabled->get<bool>();
        }
        engine.GetRenderer().SetBloom(m_bloomEnabled ? m_bloom : 0.0f);
    }

    void SettingsWindow::Save() const {
        const Json json{{"format", "flux.editor-settings"},
                        {"version", 1},
                        {"theme", UI::GetThemeNames()[static_cast<std::size_t>(m_theme)]},
                        {"volume", m_volume},
                        {"bloomEnabled", m_bloomEnabled},
                        {"bloom", m_bloom}};
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
            if (ui.Tab("Touches", m_tab == Tab::Keys)) {
                m_tab = Tab::Keys;
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
                ui.Separator();
                // Le bloom fait déborder la lumière des zones très lumineuses (soleil, flammes, émissifs).
                if (ui.Checkbox("Bloom", m_bloomEnabled)) {
                    renderer.SetBloom(m_bloomEnabled ? m_bloom : 0.0f);
                    Save();
                }
                if (m_bloomEnabled) {
                    float percent = m_bloom * 100.0f;
                    if (ui.DragFloat("Intensité du bloom (%)", percent, 0.05f, 0.0, 30.0, 1)) {
                        m_bloom = percent / 100.0f;
                        renderer.SetBloom(m_bloom);
                        m_bloomChanged = true;
                    }
                    // Enregistré au lâcher, pas à chaque pixel glissé.
                    if (m_bloomChanged && !ui.IsItemActive()) {
                        m_bloomChanged = false;
                        Save();
                    }
                }
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
            } else if (m_tab == Tab::Keys) {
                // Actions d'entrée du moteur et du jeu chargé : clic sur la touche, puis la nouvelle touche (Échap annule).
                // Gardées dans Touches.json, à côté de l'exécutable : les mêmes pour l'éditeur et le jeu.
                const InputActions& actions = engine.GetInputActions();
                std::string_view category;
                std::uint32_t row = 0;
                for (const InputAction& action : actions.GetActions()) {
                    if (action.category != category) {
                        category = action.category;
                        ui.TextDisabled(category);
                    }
                    ui.PushId(++row);
                    const bool waiting = engine.GetKeyRebinding() == action.name;
                    ui.LabelText(action.name, waiting ? std::string("Appuyez sur une touche...") : actions.GetKeysText(action.name));
                    ui.SameLine();
                    if (ui.Button("Changer")) {
                        engine.StartKeyRebinding(action.name);
                    }
                    ui.PopId();
                }
                ui.Separator();
                if (ui.Button("Touches par défaut")) {
                    engine.ResetInputActions();
                }
                ui.TextDisabled("La touche choisie remplace la principale ; la seconde reste.");
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
