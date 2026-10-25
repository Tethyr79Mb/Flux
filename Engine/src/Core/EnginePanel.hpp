#pragma once

// Panneau « Moteur » du jeu : où passe le temps d'une image (CPU, GPU, attentes), ce que dessine la scène, la mémoire,
// et le relevé des mesures dans un fichier (MeasureRecorder).

#include "Flux/Renderer/Renderer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <vector>

namespace Flux {
    struct Camera;
    class FlyCameraController;
    class MeasureRecorder;

    namespace UI {
        class Context;
    }

    using FrameClock = std::chrono::steady_clock;

    inline double Milliseconds(FrameClock::duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    }

    // Où passe le temps d'une image (millisecondes) : ce que montre le panneau « Moteur ».
    enum Cost : std::size_t {
        UpdateCost,       // CPU : fenêtre, interface, caméra, jeu, éditeur (avant le rendu)
        GameCost,         // CPU, compris dans le précédent : systèmes du jeu (physique, sièges, animation)
        PrepareCost,      // CPU : préparation du rendu
        RecordCost,       // CPU : commandes de dessin
        WaitGpuCost,      // CPU arrêté : attente du GPU
        WaitImageCost,    // CPU arrêté : attente d'une image libre et présentation (V-Sync, plafond)
        CostCount,
    };

    // Moyenne sur une fenêtre glissante d'une demi-seconde : lisible, sans chiffres qui clignotent. Un temps
    // inconnu (négatif : heures du GPU absentes) n'entre pas dans sa moyenne.
    class FrameStats {
        public:
        void Tick(FrameClock::time_point now) {
            ++m_frames;
            m_worstInWindow = std::max(m_worstInWindow, Milliseconds(now - m_lastTick));
            m_lastTick = now;
            if (now - m_worstStart >= std::chrono::seconds(5)) {
                m_previousWorst = m_worstInWindow;
                m_worstInWindow = 0.0;
                m_worstStart = now;
            }
            const std::chrono::duration<double> elapsed = now - m_windowStart;
            if (elapsed.count() >= 0.5) {
                m_fps = m_frames / elapsed.count();
                m_frameMilliseconds = 1000.0 * elapsed.count() / m_frames;
                for (std::size_t i = 0; i < CostCount; ++i) {
                    m_averages[i] = m_counts[i] > 0 ? m_sums[i] / m_counts[i] : -1.0;
                }
                for (Row& row : m_gpu) {
                    row.average = row.count > 0 ? row.sum / row.count : -1.0;
                    row.sum = 0.0;
                    row.count = 0;
                }
                m_sums = {};
                m_counts = {};
                m_frames = 0;
                m_windowStart = now;
            }
        }

        void Add(Cost cost, double milliseconds) {
            if (milliseconds >= 0.0) {
                m_sums[cost] += milliseconds;
                ++m_counts[cost];
            }
        }

        // Passes et étapes du GPU (RenderTimings::gpu) : leur liste ne change pas d'une image à l'autre.
        void AddGpu(std::span<const RenderPassTiming> passes) {
            m_gpu.resize(passes.size());
            for (std::size_t i = 0; i < passes.size(); ++i) {
                m_gpu[i].name = passes[i].name;
                m_gpu[i].step = passes[i].step;
                if (passes[i].milliseconds >= 0.0) {
                    m_gpu[i].sum += passes[i].milliseconds;
                    ++m_gpu[i].count;
                }
            }
        }

        double GetFps() const { return m_fps; }
        double GetFrameMilliseconds() const { return m_frameMilliseconds; }
        double Get(Cost cost) const { return m_averages[cost]; }
        // La plus longue image des 5 à 10 dernières secondes : un à-coup (chargement) que la moyenne cache.
        double GetWorstMilliseconds() const { return std::max(m_previousWorst, m_worstInWindow); }
        struct Row
        {
            const char* name = "";
            bool step = false;
            double sum = 0.0;
            int count = 0;
            double average = -1.0;
        };
        std::span<const Row> GetGpu() const { return m_gpu; }

        private:
        FrameClock::time_point m_windowStart = FrameClock::now();
        FrameClock::time_point m_lastTick = FrameClock::now();
        FrameClock::time_point m_worstStart = FrameClock::now();
        double m_worstInWindow = 0.0;
        double m_previousWorst = 0.0;
        int m_frames = 0;
        double m_fps = 0.0;
        double m_frameMilliseconds = 0.0;
        std::array<double, CostCount> m_sums{};
        std::array<int, CostCount> m_counts{};
        std::array<double, CostCount> m_averages{};
        std::vector<Row> m_gpu;
    };

    void DrawEnginePanel(UI::Context& ui, Renderer& renderer, const FrameStats& stats, MeasureRecorder& measures,
                         Camera& camera, FlyCameraController* controller);
}
