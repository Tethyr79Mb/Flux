#include "Panels/WorldWindow.hpp"

#include "Document/UndoHistory.hpp"
#include "Panels/SceneEditor.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/UI/Context.hpp>

namespace FluxEditor {
    using namespace Flux;

    void WorldWindow::Draw(Engine& engine, SceneEditor& sceneEditor, UndoHistory& history, bool readOnly) {
        if (!m_open) {
            m_gesture = false;
            return;
        }
        UI::Context& ui = engine.GetUi();
        Scene& scene = engine.GetScene();
        const ComponentInfo* info = engine.GetComponents().Find("world");
        if (!info) {
            return;
        }
        Entity entity = NullEntity;
        for (const auto [found, world] : scene.GetRegistry().view<const WorldComponent>().each()) {
            entity = found; // un seul compte, comme pour la physique
            break;
        }
        // Avant le dessin des champs, qui modifie l'objet : son état d'avant le geste.
        if (!m_gesture) {
            m_gestureEntity = entity != NullEntity ? scene.GetId(entity) : Uuid{};
            m_before = entity != NullEntity ? UndoHistory::Capture(engine, m_gestureEntity) : std::string{};
        }

        const Math::Vector2i screen = engine.GetWindow().GetSize();
        constexpr float Width = 380.0f;
        // Au milieu de l'écran à la première ouverture ; ensuite là où on l'a laissée.
        if (ui.BeginPanel("Monde", {(static_cast<float>(screen.x) - Width) * 0.5f, static_cast<float>(screen.y) * 0.25f},
                          Width, 0.0f, &m_open)) {
            ui.TextDisabled(entity != NullEntity ? "Réglages de cette scène, hors des planètes."
                                                 : "Valeurs par défaut : modifiées, elles sont gardées dans la scène.");
            ui.Separator();
            ui.BeginDisabled(readOnly);
            void* component = entity != NullEntity ? info->get(scene.GetRegistry(), entity) : &m_defaults;
            if (sceneEditor.DrawFields(engine, *info, component) && !readOnly) {
                if (entity == NullEntity) {
                    // Première modification : l'objet Monde est créé avec ces valeurs (m_before reste vide).
                    history.Commit(engine); // un geste en cours dans l'inspecteur est un pas à part
                    entity = scene.CreateEntity("Monde");
                    scene.Add<WorldComponent>(entity, m_defaults);
                    m_defaults = {};
                    m_gestureEntity = scene.GetId(entity);
                }
                m_gesture = true;
            }
            ui.EndDisabled();
        }
        ui.EndPanel();

        if (m_gesture && !ui.IsInteracting()) {
            m_gesture = false;
            history.Push("Réglages du monde", m_gestureEntity, std::move(m_before), UndoHistory::Capture(engine, m_gestureEntity));
            m_before.clear();
        }
    }
}
