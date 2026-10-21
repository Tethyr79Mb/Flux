#include "Document/PlayMode.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Core/Log.hpp>
#include <Flux/Platform/Dialogs.hpp>
#include <Flux/Platform/Input.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Scene/SceneSerializer.hpp>
#include <Flux/UI/Context.hpp>

#include <cmath>
#include <string>

namespace FluxEditor {
    using namespace Flux;

    void PlayMode::DrawToolbar(UI::Context& ui, Engine& engine) {
        // Au milieu de la barre d'outils : Jouer (ou Arrêter pendant le jeu) et Pause, en pictogrammes seuls ; leur nom
        // et leur raccourci sont dans l'info-bulle.
        const float width = 2.0f * ui.GetBarItemHeight() + 2.0f; // deux boutons carrés, 2 pixels entre eux
        ui.SetMenuBarCursor(std::round((static_cast<float>(engine.GetWindow().GetSize().x) - width) * 0.5f));
        if (ui.MenuBarButton("##jouer", m_playing, m_playing ? UI::Icon::Stop : UI::Icon::Play)) {
            m_playing ? Stop(engine) : Start(engine);
        }
        ui.SetItemTooltip(m_playing ? "Arrêter le jeu (F5)" : "Jouer la scène (F5)");
        if (ui.MenuBarButton("##pause", engine.GetGameState() == GameState::Paused, UI::Icon::Pause) && m_playing) {
            TogglePause(engine);
        }
        ui.SetItemTooltip(m_playing ? "Pause (F6)" : "Pause (F6), pendant le jeu");
    }

    void PlayMode::HandleShortcuts(Engine& engine) {
        const Input& input = engine.GetWindow().GetInput();
        if (engine.GetUi().WantsKeyboard()) {
            return;
        }
        if (input.WasKeyPressed(Key::F5)) {
            m_playing ? Stop(engine) : Start(engine);
        } else if (input.WasKeyPressed(Key::F6) && m_playing) {
            TogglePause(engine);
        }
    }

    void PlayMode::Start(Engine& engine) {
        if (m_playing) {
            return;
        }
        // Copie par la sauvegarde : tout ce qui s'enregistre revient à l'identique, identifiants compris
        // (la sélection de l'éditeur survit), composants de types inconnus aussi.
        m_savedScene = SceneSerializer::SaveToString(engine.GetScene(), engine.GetComponents(), engine.GetPrefabReader());
        m_savedCamera = engine.GetCamera();
        m_playing = true;
        engine.SetGameState(GameState::Running);
        // Comme le jeu : une partie sauvegardée (F8) reprend où elle était ; la scène éditée revient à l'arrêt.
        engine.ResumeQuickSave();
        Log::Info("jeu lancé");
    }

    void PlayMode::Stop(Engine& engine) {
        if (!m_playing) {
            return;
        }
        engine.SetGameState(GameState::Stopped);
        m_playing = false;
        if (auto restored = SceneSerializer::LoadFromString(engine.GetScene(), engine.GetComponents(), m_savedScene,
                                                              engine.GetPrefabReader());
            !restored) {
            // Ne devrait pas arriver (le texte vient de SaveToString) ; la scène du jeu reste alors en place.
            Platform::ShowError(engine.GetWindow(), "Arrêter le jeu", "Scène d'origine non restaurée : " + restored.error());
        }
        engine.GetCamera() = m_savedCamera;
        m_savedScene.clear();
        Log::Info("jeu arrêté : retour à la scène éditée");
    }

    void PlayMode::TogglePause(Engine& engine) {
        engine.SetGameState(engine.GetGameState() == GameState::Paused ? GameState::Running : GameState::Paused);
    }
}
