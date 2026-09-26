#include "PlayMode.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Dialogs.hpp>
#include <Flux/Platform/Input.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Scene/SceneSerializer.hpp>
#include <Flux/UI/Context.hpp>

namespace FluxEditor {
    using namespace Flux;

    void PlayMode::DrawToolbar(UI::Context& ui, Engine& engine) {
        const char* play = m_playing ? "Arrêter (F5)" : "Jouer (F5)";
        const char* pause = "Pause (F6)";
        const float spacing = ui.GetStyle().itemSpacing;
        const float padding = 2.0f * ui.GetStyle().framePadding.x;
        const float width = ui.MeasureText(play).x + ui.MeasureText(pause).x + 2.0f * padding + spacing;
        ui.SetMenuBarCursor((static_cast<float>(engine.GetWindow().GetSize().x) - width) * 0.5f);
        // Libellés à identifiant fixe : le bouton reste le même widget quand son texte change.
        if (ui.MenuBarButton(std::string(play) + "##jouer", m_playing)) {
            m_playing ? Stop(engine) : Start(engine);
        }
        if (ui.MenuBarButton(std::string(pause) + "##pause", engine.GetGameState() == GameState::Paused) && m_playing) {
            TogglePause(engine);
        }
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
        m_savedScene = SceneSerializer::SaveToString(engine.GetScene(), engine.GetComponents());
        m_savedCamera = engine.GetCamera();
        m_playing = true;
        engine.SetGameState(GameState::Running);
    }

    void PlayMode::Stop(Engine& engine) {
        if (!m_playing) {
            return;
        }
        engine.SetGameState(GameState::Stopped);
        m_playing = false;
        if (auto restored = SceneSerializer::LoadFromString(engine.GetScene(), engine.GetComponents(), m_savedScene);
            !restored) {
            // Ne devrait pas arriver (le texte vient de SaveToString) ; la scène du jeu reste alors en place.
            Platform::ShowError(engine.GetWindow(), "Arrêter le jeu", "Scène d'origine non restaurée : " + restored.error());
        }
        engine.GetCamera() = m_savedCamera;
        m_savedScene.clear();
    }

    void PlayMode::TogglePause(Engine& engine) {
        engine.SetGameState(engine.GetGameState() == GameState::Paused ? GameState::Running : GameState::Paused);
    }
}
