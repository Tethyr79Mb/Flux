#include "Hud.hpp"

#include "GameSim.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Renderer/Renderer.hpp>
#include <Flux/Scene/Picking.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/UI/DrawList.hpp>
#include <Flux/UI/HudText.hpp>

#include <string>

namespace Game {
    namespace {
        using namespace Flux;

        // Cible verrouillée : quatre coins autour d'elle et sa distance.
        void DrawLock(Engine& engine, double) {
            const Scene& scene = engine.GetScene();
            const PilotWeaponsComponent* state = PlayerWeapons::FindState(scene, engine.GetPlayer());
            const Entity lockTarget = state ? state->lockTarget : NullEntity;
            if (lockTarget == NullEntity || !scene.IsValid(lockTarget)) {
                return;
            }
            const Camera& camera = engine.GetCamera();
            const Math::Vector2i size = engine.GetWindow().GetSize();
            const Math::Vector2f screen{static_cast<float>(size.x), static_cast<float>(size.y)};
            const Math::Vector3d offset = Math::Delta(camera.position, scene.GetWorldPosition(lockTarget));
            const auto point = ScreenProjection(camera, {}, screen).ToScreen(offset);
            if (!point) {
                return;
            }
            UI::DrawList& list = engine.GetRenderer().GetOverlay();
            constexpr float Half = 22.0f;
            constexpr float Corner = 9.0f;
            constexpr Math::LinearColor LockColor{1.0f, 0.35f, 0.2f, 0.95f};
            for (const float sx : {-1.0f, 1.0f}) {
                for (const float sy : {-1.0f, 1.0f}) {
                    const Math::Vector2f tip{point->x + sx * Half, point->y + sy * Half};
                    list.AddLine(tip, {tip.x - sx * Corner, tip.y}, LockColor, 2.0f);
                    list.AddLine(tip, {tip.x, tip.y - sy * Corner}, LockColor, 2.0f);
                }
            }
            const std::string label = FormatDistance(Math::Length(offset));
            constexpr float LabelSize = 15.0f;
            const Math::Vector2f labelExtent = list.MeasureText(label, LabelSize);
            list.AddText({point->x - labelExtent.x * 0.5f, point->y + Half + 4.0f}, label, LabelSize, LockColor);
        }

        constexpr const char* HudSystem = "Armes du joueur : affichage";
    }

    void RegisterPlayerWeaponsHud(Engine& engine) {
        engine.AddGameSystem(Engine::GameStage::Hud, HudSystem, DrawLock);
    }

    void UnregisterPlayerWeaponsHud(Engine& engine) {
        engine.RemoveGameSystem(HudSystem);
    }
}
