#pragma once

// Mesures du panneau Moteur enregistrées pendant quelques secondes, puis écrites dans un fichier texte lisible
// (Mesures/ à côté de l'exécutable) : moyenne, minimum et maximum de chaque valeur, et la répartition des durées
// d'image. Un relevé qu'on peut comparer d'une version à l'autre ou envoyer, au lieu de captures d'écran.

#include "Flux/Renderer/Renderer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Flux {
    class MeasureRecorder {
        public:
        // Temps du CPU d'une image (ms, négatif : inconnu), dans l'ordre du panneau.
        enum Cpu : std::size_t { Update, Game, Prepare, Record, WaitGpu, WaitImage, CpuCount };

        // Ce qui décrit le relevé, en tête du fichier.
        struct Context
        {
            std::string_view device;
            std::string camera;     // position et direction
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            bool vsync = false;
            bool temporalAA = false;
            GroundDetail groundDetail = DefaultGroundDetail;
            bool occlusionCulling = false;  // « Objets cachés écartés »
            bool groundGeometryOnly = false; // « Sol sans pixels »
            std::uint32_t groundMeshThreads = 0; // 0 : sol par les sommets
            bool modelMeshShaders = false;       // modèles fixes en mesh shaders
        };

        void Start(double seconds);
        bool IsRecording() const { return m_recording; }
        double GetRemainingSeconds() const { return m_recording ? std::max(m_duration - m_elapsed, 0.0) : 0.0; }
        // Une image : sa durée (ms) et ses mesures. Retourne true quand la durée demandée est atteinte (Write).
        bool Add(double frameMilliseconds, const std::array<double, CpuCount>& cpu, const RenderTimings& render);
        // Écrit le relevé dans `directory` (créé au besoin) et arrête l'enregistrement : chemin du fichier.
        std::expected<std::filesystem::path, std::string> Write(const std::filesystem::path& directory, const Context& context);

        private:
        struct Stat
        {
            std::string name;
            int depth = 0; // retrait dans le fichier (étape d'une passe du GPU)
            double sum = 0.0;
            double min = 0.0;
            double max = 0.0;
            int count = 0;
            void Add(double value);
        };
        Stat& GpuStat(std::size_t index, const RenderPassTiming& pass);
        void Reset();

        bool m_recording = false;
        double m_duration = 0.0;
        double m_elapsed = 0.0;
        // Gardées une à une pour la médiane et les pires (réservées au départ : pas d'allocation en cours de route
        // sous 64k images).
        std::vector<double> m_frames;
        std::array<Stat, CpuCount> m_cpu;
        std::vector<Stat> m_gpu; // passes et étapes, dans l'ordre de l'image
        static constexpr std::size_t CounterCount = 16;
        std::array<Stat, CounterCount> m_counters;
    };
}
