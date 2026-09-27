#include "UndoHistory.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Dialogs.hpp>
#include <Flux/Scene/SceneSerializer.hpp>

namespace FluxEditor {
    using namespace Flux;

    std::string UndoHistory::Capture(Engine& engine, Uuid entity, bool withDescendants) {
        const Scene& scene = engine.GetScene();
        const Entity found = entity ? scene.FindById(entity) : NullEntity;
        return found == NullEntity ? std::string{}
                                   : SceneSerializer::SaveEntity(scene, engine.GetComponents(), found, withDescendants);
    }

    void UndoHistory::BeginFrame(Engine& engine, Uuid selection) {
        m_frameModified = false;
        if (!m_enabled || m_gestureOpen) {
            return;
        }
        // État de la sélection avant les panneaux : si l'un d'eux la modifie, c'est l'état d'avant le geste.
        // Une seule entité, une fois par frame : négligeable.
        m_gestureEntity = selection;
        m_gestureBefore = selection ? Capture(engine, selection) : std::string{};
    }

    void UndoHistory::EndFrame(Engine& engine, bool interacting) {
        if (!m_enabled) {
            return;
        }
        if (m_frameModified && m_gestureEntity) {
            m_gestureOpen = true;
        }
        if (m_gestureOpen && !interacting) {
            Commit(engine);
        }
    }

    void UndoHistory::Commit(Engine& engine) {
        if (!m_gestureOpen) {
            return;
        }
        m_gestureOpen = false;
        Push(m_gestureLabel, m_gestureEntity, std::move(m_gestureBefore), Capture(engine, m_gestureEntity));
        m_gestureBefore.clear();
    }

    void UndoHistory::Push(std::string label, Uuid entity, std::string before, std::string after) {
        if (!m_enabled || before == after) {
            return;
        }
        m_steps.erase(m_steps.begin() + static_cast<std::ptrdiff_t>(m_position), m_steps.end()); // plus de refaire
        m_steps.push_back({std::move(label), entity, std::move(before), std::move(after)});
        if (m_steps.size() > MaxSteps) {
            m_steps.pop_front();
        }
        m_position = m_steps.size();
        m_changed = true;
    }

    Uuid UndoHistory::Undo(Engine& engine) {
        Commit(engine);
        if (!m_enabled || !CanUndo()) {
            return {};
        }
        const Step& step = m_steps[--m_position];
        return Apply(engine, step.entity, step.before);
    }

    Uuid UndoHistory::Redo(Engine& engine) {
        Commit(engine);
        if (!m_enabled || !CanRedo()) {
            return {};
        }
        const Step& step = m_steps[m_position++];
        return Apply(engine, step.entity, step.after);
    }

    Uuid UndoHistory::Apply(Engine& engine, Uuid entity, const std::string& snapshot) {
        Scene& scene = engine.GetScene();
        m_changed = true;
        m_gestureOpen = false;
        if (snapshot.empty()) {
            scene.DestroyEntity(scene.FindById(entity));
            return {};
        }
        if (auto restored = SceneSerializer::RestoreEntity(scene, engine.GetComponents(), snapshot); !restored) {
            // Ne devrait pas arriver : les pas se défont dans l'ordre, le parent d'une entité existe donc toujours.
            Platform::ShowError(engine.GetWindow(), "Annuler", "Modification non annulée : " + restored.error());
            return {};
        }
        return entity;
    }

    void UndoHistory::Clear() {
        m_steps.clear();
        m_position = 0;
        m_gestureOpen = false;
        m_gestureBefore.clear();
    }
}
