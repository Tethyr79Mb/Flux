#include "Core/LoadingScreen.hpp"

#include "Flux/Core/Log.hpp"
#include "Flux/Renderer/Renderer.hpp"
#include "Flux/UI/DrawList.hpp"

#include <algorithm>
#include <string_view>

namespace Flux {
    namespace {
        // Plus rien d'attendu pendant quelques images : les tuiles prêtes en font demander de plus fines tant que le
        // sol n'est pas au détail voulu, une seule image vide ne suffit pas. Au-delà de MaxSeconds, la scène est
        // montrée quand même (une tuile qui ne viendrait jamais ne doit pas bloquer le jeu).
        constexpr std::uint32_t QuietFrames = 10;
        constexpr double MinSeconds = 0.2;
        constexpr double MaxSeconds = 30.0;

        std::uint32_t CountPending(const RenderTimings& render) {
            return render.planetPendingTiles + (render.groundTexturesLoading ? 1u : 0u) + render.modelsLoading
                 + render.fontsLoading + render.pendingTreePlains;
        }
    }

    void LoadingScreen::Begin() {
        if (!m_enabled) {
            return;
        }
        m_active = true;
        m_seconds = 0.0;
        m_quietFrames = 0;
        m_mostPending = 0;
    }

    void LoadingScreen::Update(const RenderTimings& render, double deltaSeconds) {
        if (!m_active) {
            return;
        }
        const std::uint32_t pending = CountPending(render);
        m_mostPending = std::max(m_mostPending, pending);
        m_quietFrames = pending == 0 ? m_quietFrames + 1 : 0;
        m_seconds += deltaSeconds;
        if (m_seconds >= MaxSeconds) {
            Log::Warning("Chargement : scène montrée après {:.0f} s, {} tuiles ou modèles encore attendus", MaxSeconds, pending);
        } else if (m_quietFrames < QuietFrames || m_seconds < MinSeconds) {
            return;
        }
        m_active = false;
        Log::Info("Scène prête en {:.1f} s", m_seconds);
    }

    void LoadingScreen::Draw(UI::DrawList& list, const RenderTimings& render, Math::Vector2f screen) const {
        list.AddRect({0.0f, 0.0f}, screen, {0.012f, 0.014f, 0.02f, 1.0f});
        constexpr float TextSize = 28.0f;
        const std::string_view text = "Chargement..."; // police en Latin-1 : pas de « … »
        const Math::Vector2f extent = list.MeasureText(text, TextSize);
        const Math::Vector2f center = screen * 0.5f;
        list.AddText({center.x - extent.x * 0.5f, center.y - extent.y - 16.0f}, text, TextSize, {0.85f, 0.88f, 0.95f, 1.0f});
        // Avancement : la part des attentes déjà levées, puis les dernières images de vérification.
        const std::uint32_t pending = CountPending(render);
        const float done = m_mostPending > 0 ? 1.0f - static_cast<float>(pending) / static_cast<float>(m_mostPending) : 1.0f;
        const float progress = std::clamp(0.9f * done + 0.1f * static_cast<float>(m_quietFrames) / static_cast<float>(QuietFrames),
                                          0.0f, 1.0f);
        constexpr float BarWidth = 320.0f;
        constexpr float BarHeight = 4.0f;
        const Math::Vector2f barMin{center.x - BarWidth * 0.5f, center.y + 8.0f};
        list.AddRect(barMin, {barMin.x + BarWidth, barMin.y + BarHeight}, {1.0f, 1.0f, 1.0f, 0.12f});
        list.AddRect(barMin, {barMin.x + BarWidth * progress, barMin.y + BarHeight}, {0.55f, 0.75f, 1.0f, 1.0f});
    }
}
