#include "Core/Notice.hpp"

#include "Flux/UI/DrawList.hpp"

#include <algorithm>

namespace Flux {
    namespace {
        constexpr double ShownSeconds = 2.5;
    }

    void Notice::Show(std::string text) {
        m_text = std::move(text);
        m_seconds = ShownSeconds;
    }

    void Notice::Draw(UI::DrawList& list, Math::Vector2f screen, double deltaSeconds, bool loading) {
        if (!loading) {
            m_seconds -= deltaSeconds; // l'écran de chargement le cache : il attend la fin pour décompter
        }
        // En haut au centre, au-dessus de tout ; s'efface dans la dernière demi-seconde.
        constexpr float NoticeSize = 22.0f;
        const float alpha = static_cast<float>(std::clamp(m_seconds / 0.5, 0.0, 1.0));
        const Math::Vector2f extent = list.MeasureText(m_text, NoticeSize);
        const Math::Vector2f position{(screen.x - extent.x) * 0.5f, screen.y * 0.08f};
        list.AddText({position.x + 1.5f, position.y + 1.5f}, m_text, NoticeSize, {0.0f, 0.0f, 0.0f, 0.6f * alpha});
        list.AddText(position, m_text, NoticeSize, {1.0f, 1.0f, 1.0f, alpha});
    }
}
