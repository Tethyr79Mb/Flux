#include "ContentBrowser.hpp"

#include "SceneDocument.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Dialogs.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/UI/Context.hpp>

#include <algorithm>
#include <system_error>
#include <utility>

namespace FluxEditor {
    using namespace Flux;

    namespace {
        constexpr double ScanInterval = 2.0; // secondes : quelques dizaines de fichiers, lecture négligeable

        std::string ToUtf8(const std::filesystem::path& path) {
            const std::u8string text = path.u8string();
            return {text.begin(), text.end()};
        }

        bool IsScene(const std::filesystem::path& path) {
            return ToUtf8(path.filename()).ends_with(".scene.json");
        }
    }

    void ContentBrowser::Update(Engine& engine, SceneDocument& document, double deltaSeconds) {
        const Project* project = engine.GetProject();
        const std::filesystem::path root = project ? project->assetsDirectory : std::filesystem::path{};
        m_sinceScan += deltaSeconds;
        if (root != m_root || m_sinceScan >= ScanInterval) {
            m_root = root;
            m_sinceScan = 0.0;
            Scan(root);
        }

        UI::Context& ui = engine.GetUi();
        const Math::Vector2i screen = engine.GetWindow().GetSize();
        // À droite, sous le panneau « Démo » du jeu, jusqu'en bas de la fenêtre.
        const float top = 150.0f + ui.GetMenuBarHeight();
        if (ui.BeginPanel("Contenu", {std::max(316.0f, static_cast<float>(screen.x) - 368.0f), top}, 360.0f,
                          std::max(160.0f, static_cast<float>(screen.y) - top - 8.0f))) {
            if (!project) {
                ui.TextDisabled("Aucun projet ouvert.");
            } else if (m_entries.empty()) {
                ui.TextDisabled("Dossier des assets vide ou introuvable :\n" + ToUtf8(root));
            } else {
                ui.TextDisabled("Double-clic sur une scène pour l'ouvrir,\nclic droit pour les autres actions.");
                ui.Separator();
                for (const Entry& entry : m_entries) {
                    DrawEntry(ui, engine, document, entry);
                }
            }
        }
        ui.EndPanel();
    }

    void ContentBrowser::Scan(const std::filesystem::path& root) {
        // Récursif ; erreurs ignorées (fichier supprimé pendant la lecture, droits) : l'entrée manque, c'est tout.
        const auto read = [](const auto& self, const std::filesystem::path& directory) -> std::vector<Entry> {
            std::vector<Entry> entries;
            std::error_code error;
            for (auto it = std::filesystem::directory_iterator(directory, error);
                 !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
                Entry entry;
                entry.path = it->path();
                entry.name = ToUtf8(entry.path.filename());
                entry.directory = it->is_directory(error);
                if (entry.directory) {
                    entry.children = self(self, entry.path);
                }
                entries.push_back(std::move(entry));
            }
            // Dossiers d'abord, puis ordre alphabétique : le même d'une lecture à l'autre.
            std::ranges::sort(entries, [](const Entry& a, const Entry& b) {
                return a.directory != b.directory ? a.directory : a.name < b.name;
            });
            return entries;
        };
        m_entries = root.empty() ? std::vector<Entry>{} : read(read, root);
    }

    void ContentBrowser::DrawEntry(UI::Context& ui, Engine& engine, SceneDocument& document, const Entry& entry) {
        ui.PushId(entry.name);
        if (entry.directory) {
            const bool open = ui.TreeNode(entry.name + "/", false, entry.children.empty(), true).open;
            ShowContextMenu(ui, engine, document, entry);
            if (open) {
                for (const Entry& child : entry.children) {
                    DrawEntry(ui, engine, document, child);
                }
                ui.TreePop();
            }
        } else {
            const bool current = document.GetPath().lexically_normal() == entry.path.lexically_normal();
            const UI::Context::TreeNodeResult node = ui.TreeNode(entry.name, current, true);
            if (node.doubleClicked && IsScene(entry.path) && !current) {
                document.OpenFile(engine, entry.path);
            }
            ShowContextMenu(ui, engine, document, entry);
        }
        ui.PopId();
    }

    void ContentBrowser::ShowContextMenu(UI::Context& ui, Engine& engine, SceneDocument& document, const Entry& entry) {
        if (!ui.BeginContextMenu()) {
            return;
        }
        const bool scene = !entry.directory && IsScene(entry.path);
        if (scene && ui.MenuItem("Ouvrir")) {
            document.OpenFile(engine, entry.path);
        }
        if (ui.MenuItem("Afficher dans l'explorateur")) {
            Platform::ShowInExplorer(entry.path);
        }
        // Les dossiers se suppriment dans l'explorateur : trop facile de perdre beaucoup d'un coup ici.
        if (!entry.directory && ui.MenuItem("Supprimer")) {
            Delete(engine, document, entry);
        }
        ui.EndMenu();
    }

    void ContentBrowser::Delete(Engine& engine, SceneDocument& document, const Entry& entry) {
        const Window& window = engine.GetWindow();
        const Project* project = engine.GetProject();
        if (project && project->GetStartupScenePath().lexically_normal() == entry.path.lexically_normal()) {
            Platform::ShowError(window, "Supprimer", "« " + entry.name + " » est la scène de démarrage du projet : "
                                                     "Game.exe ne pourrait plus démarrer.");
            return;
        }
        if (!Platform::AskYesNo(window, "Supprimer", "Envoyer « " + entry.name + " » à la corbeille ?")) {
            return;
        }
        if (auto deleted = Platform::MoveToRecycleBin(entry.path); !deleted) {
            Platform::ShowError(window, "Supprimer", deleted.error());
            return;
        }
        document.ForgetFile(entry.path);
        m_sinceScan = ScanInterval; // relu à la prochaine frame
    }
}
