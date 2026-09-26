#include "SceneDocument.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Dialogs.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Input.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/Scene/SceneSerializer.hpp>
#include <Flux/UI/Context.hpp>

#include <system_error>

namespace FluxEditor {
    using namespace Flux;

    namespace {
        const Platform::FileType SceneFileType{"Scènes Flux", ".scene.json"};

        // path::string() convertit dans la page de code Windows : les accents seraient perdus.
        std::string FileName(const std::filesystem::path& path) {
            const std::u8string name = path.filename().u8string();
            return {name.begin(), name.end()};
        }
    }

    void SceneDocument::Update(Engine& engine) {
        UI::Context& ui = engine.GetUi();

        // Raccourcis (touches physiques : S, O et N sont au même endroit en AZERTY et en QWERTY).
        // Muets pendant une saisie : Ctrl+S dans un champ texte ne doit pas enregistrer à moitié.
        const Input& input = engine.GetWindow().GetInput();
        if (!ui.WantsKeyboard() && input.IsControlDown()) {
            if (input.WasKeyPressed(Key::N)) {
                NewScene(engine);
            } else if (input.WasKeyPressed(Key::O)) {
                Open(engine);
            } else if (input.WasKeyPressed(Key::S)) {
                input.IsShiftDown() ? SaveAs(engine) : Save(engine);
            }
        }

        if (ui.BeginMainMenuBar()) {
            if (ui.BeginMenu("Fichier")) {
                if (ui.MenuItem("Nouvelle scène", "Ctrl+N")) {
                    NewScene(engine);
                }
                if (ui.MenuItem("Ouvrir...", "Ctrl+O")) {
                    Open(engine);
                }
                if (ui.MenuItem("Enregistrer", "Ctrl+S")) {
                    Save(engine);
                }
                if (ui.MenuItem("Enregistrer sous...", "Ctrl+Maj+S")) {
                    SaveAs(engine);
                }
                ui.Separator();
                if (ui.MenuItem("Quitter", "Alt+F4") && ConfirmClose(engine)) {
                    engine.RequestQuit();
                }
                ui.EndMenu();
            }
            ui.EndMainMenuBar();
        }
        UpdateTitle(engine);
    }

    bool SceneDocument::ConfirmClose(Engine& engine) {
        return ConfirmDiscard(engine);
    }

    void SceneDocument::NewScene(Engine& engine) {
        if (!ConfirmDiscard(engine)) {
            return;
        }
        engine.GetScene().Clear();
        m_path.clear();
        m_modified = false;
    }

    void SceneDocument::Open(Engine& engine) {
        if (!ConfirmDiscard(engine)) {
            return;
        }
        const auto path = Platform::ShowOpenFileDialog(engine.GetWindow(), "Ouvrir une scène", SceneFileType, DialogDirectory());
        if (!path) {
            return;
        }
        // En cas d'erreur, la scène en cours reste intacte (le chargement vérifie tout avant de la remplacer).
        if (auto loaded = SceneSerializer::Load(engine.GetScene(), engine.GetComponents(), *path); !loaded) {
            Platform::ShowError(engine.GetWindow(), "Ouvrir une scène", loaded.error());
            return;
        }
        m_path = *path;
        m_modified = false;
    }

    bool SceneDocument::Save(Engine& engine) {
        return m_path.empty() ? SaveAs(engine) : WriteTo(engine, m_path);
    }

    bool SceneDocument::SaveAs(Engine& engine) {
        const std::string name = m_path.empty() ? "Nouvelle.scene.json" : FileName(m_path);
        const auto path = Platform::ShowSaveFileDialog(engine.GetWindow(), "Enregistrer la scène", SceneFileType,
                                                       DialogDirectory(), name);
        return path && WriteTo(engine, *path);
    }

    bool SceneDocument::WriteTo(Engine& engine, const std::filesystem::path& path) {
        if (auto saved = SceneSerializer::Save(engine.GetScene(), engine.GetComponents(), path); !saved) {
            Platform::ShowError(engine.GetWindow(), "Enregistrer la scène", saved.error());
            return false;
        }
        m_path = path;
        m_modified = false;
        return true;
    }

    bool SceneDocument::ConfirmDiscard(Engine& engine) {
        if (!m_modified) {
            return true;
        }
        const std::string name = m_path.empty() ? "la scène sans titre" : "« " + FileName(m_path) + " »";
        switch (Platform::AskYesNoCancel(engine.GetWindow(), "Flux Editor",
                                         "Enregistrer les modifications de " + name + " ?")) {
            case Platform::Answer::Yes:
                return Save(engine);
            case Platform::Answer::No:
                return true;
            case Platform::Answer::Cancel:
                break;
        }
        return false;
    }

    void SceneDocument::UpdateTitle(Engine& engine) {
        std::string title = m_path.empty() ? "Sans titre" : FileName(m_path);
        if (m_modified) {
            title += " *";
        }
        title += " - Flux Editor";
        if (title != m_title) {
            engine.GetWindow().SetTitle(title);
            m_title = std::move(title);
        }
    }

    std::filesystem::path SceneDocument::DialogDirectory() const {
        if (!m_path.empty()) {
            return m_path.parent_path();
        }
        // Les scènes du dépôt plutôt que leur copie dans bin/, écrasée à chaque build qui touche aux assets.
        std::error_code error;
        const std::filesystem::path sources(u8"" FLUX_SOURCE_SCENES_DIR);
        if (std::filesystem::is_directory(sources, error)) {
            return sources;
        }
        return Platform::GetExecutableDirectory() / "assets" / "scenes";
    }
}
