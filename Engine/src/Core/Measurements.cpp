#include "Core/Measurements.hpp"

#include "Flux/Core/Version.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <cmath>
#include <ctime>
#include <format>
#include <fstream>

namespace Flux {
    namespace {
        constexpr std::array<const char*, MeasureRecorder::CpuCount> CpuNames{
            "CPU mise à jour", "  dont jeu", "CPU préparation du rendu", "CPU commandes", "Attente du GPU", "Attente de l'écran"};

        std::tm LocalTime() {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            return local;
        }

        std::string Time(const std::tm& time, const char* format) {
            char text[32];
            std::strftime(text, sizeof(text), format, &time);
            return text;
        }

        // Valeur de rang `fraction` (0 à 1) des durées triées.
        double Quantile(const std::vector<double>& sorted, double fraction) {
            const auto index = static_cast<std::size_t>(fraction * static_cast<double>(sorted.size() - 1) + 0.5);
            return sorted[std::min(index, sorted.size() - 1)];
        }
    }

    void MeasureRecorder::Stat::Add(double value) {
        if (value < 0.0) {
            return; // inconnu (heures du GPU pas encore relues)
        }
        min = count == 0 ? value : std::min(min, value);
        max = count == 0 ? value : std::max(max, value);
        sum += value;
        ++count;
    }

    void MeasureRecorder::Reset() {
        m_elapsed = 0.0;
        m_frames.clear();
        for (std::size_t i = 0; i < CpuCount; ++i) {
            m_cpu[i] = {CpuNames[i]};
        }
        m_gpu.clear();
        constexpr std::array<const char*, CounterCount> counters{
            "Tuiles de planète", "  dont eau", "  gardées", "  calculées", "  en attente", "Arbres près",
            "Arbres simplifiés", "Arbres loin", "Ombres d'arbres", "Plaines d'arbres", "  à semer", "  semées",
            "Objets testés", "  dont cachés", "Paquets des modèles", "  dont dessinés"};
        for (std::size_t i = 0; i < CounterCount; ++i) {
            m_counters[i] = {counters[i]};
        }
    }

    void MeasureRecorder::Start(double seconds) {
        Reset();
        m_frames.reserve(65536);
        m_gpu.reserve(64);
        m_duration = seconds;
        m_recording = true;
    }

    MeasureRecorder::Stat& MeasureRecorder::GpuStat(std::size_t index, const RenderPassTiming& pass) {
        // Par place, pas par nom : une étape peut porter le nom d'une passe (« scène » de l'image finale). La liste
        // ne change pas d'une image à l'autre ; si un réglage la change en cours de relevé, la ligne repart de zéro.
        const int depth = pass.step ? 1 : 0;
        if (index >= m_gpu.size()) {
            return m_gpu.emplace_back(Stat{pass.name, depth});
        }
        if (m_gpu[index].name != pass.name || m_gpu[index].depth != depth) {
            m_gpu[index] = {pass.name, depth};
        }
        return m_gpu[index];
    }

    bool MeasureRecorder::Add(double frameMilliseconds, const std::array<double, CpuCount>& cpu, const RenderTimings& render) {
        if (!m_recording) {
            return false;
        }
        m_elapsed += frameMilliseconds * 0.001;
        m_frames.push_back(frameMilliseconds);
        for (std::size_t i = 0; i < CpuCount; ++i) {
            m_cpu[i].Add(cpu[i]);
        }
        for (std::size_t i = 0; i < render.gpu.size(); ++i) {
            GpuStat(i, render.gpu[i]).Add(render.gpu[i].milliseconds);
        }
        const std::array<std::uint32_t, CounterCount> counters{
            render.planetTiles, render.planetOceanTiles, render.planetStoredTiles, render.planetComputedTiles,
            render.planetPendingTiles, render.trees[0], render.trees[1], render.trees[2], render.shadowTrees,
            render.treePlains, render.pendingTreePlains, render.sownTreePlains, render.occlusionTested,
            render.occlusionHidden, render.modelMeshlets, render.modelMeshletsDrawn};
        for (std::size_t i = 0; i < CounterCount; ++i) {
            m_counters[i].Add(static_cast<double>(counters[i]));
        }
        return m_elapsed >= m_duration;
    }

    std::expected<std::filesystem::path, std::string> MeasureRecorder::Write(const std::filesystem::path& directory,
                                                                              const Context& context) {
        m_recording = false;
        if (m_frames.empty()) {
            return std::unexpected("aucune image enregistrée");
        }
        const std::tm now = LocalTime();
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        const std::filesystem::path path = directory / Platform::PathFromUtf8(std::format("mesures-{}.txt", Time(now, "%Y-%m-%d-%H%M%S")));

        std::vector<double> sorted = m_frames;
        std::ranges::sort(sorted);
        double total = 0.0;
        for (const double frame : m_frames) {
            total += frame;
        }
        const double average = total / static_cast<double>(m_frames.size());

        std::string text;
        text += std::format("Mesures de Flux {} du {}\n", GetFullVersion(), Time(now, "%d/%m/%Y à %H:%M:%S"));
        text += std::format("GPU : {}\n", context.device);
        text += std::format("Image : {} x {}, V-Sync {}, anticrénelage {}, détail du sol {}\n", context.width, context.height,
                            context.vsync ? "oui" : "non", context.temporalAA ? "oui" : "non",
                            GroundDetailLabels[static_cast<std::size_t>(context.groundDetail)]);
        text += std::format("Objets cachés écartés {}, sol sans pixels {}, sol dessiné par {}\n",
                            context.occlusionCulling ? "oui" : "non", context.groundGeometryOnly ? "oui" : "non",
                            context.groundMeshThreads > 0 ? std::format("mesh shaders ({} threads)", context.groundMeshThreads)
                                                          : std::string("sommets"));
        text += std::format("Modèles fixes dessinés par {}\n", context.modelMeshShaders ? "mesh shaders" : "sommets");
        text += std::format("Caméra : {}\n", context.camera);
        text += std::format("Durée : {:.1f} s, {} images\n\n", total * 0.001, m_frames.size());

        text += std::format("Images/s : {:.0f}\n", 1000.0 / average);
        text += std::format("Durée d'image (ms) : moyenne {:.3f}, médiane {:.3f}, 1 % les plus longues {:.3f}, "
                            "0,1 % {:.3f}, pire {:.3f}, meilleure {:.3f}\n\n",
                            average, Quantile(sorted, 0.5), Quantile(sorted, 0.99), Quantile(sorted, 0.999), sorted.back(),
                            sorted.front());

        const auto table = [&text](std::string_view title, std::span<const Stat> stats, bool integers) {
            text += std::format("{:<34}{:>10}{:>10}{:>10}\n", title, "moyenne", "min", "max");
            for (const Stat& stat : stats) {
                const std::string name = std::string(static_cast<std::size_t>(stat.depth) * 2, ' ') + stat.name;
                if (stat.count == 0) {
                    text += std::format("{:<34}{:>10}\n", name, "?");
                } else if (integers) {
                    text += std::format("{:<34}{:>10.0f}{:>10.0f}{:>10.0f}\n", name, stat.sum / stat.count, stat.min, stat.max);
                } else {
                    text += std::format("{:<34}{:>10.3f}{:>10.3f}{:>10.3f}\n", name, stat.sum / stat.count, stat.min, stat.max);
                }
            }
            text += '\n';
        };
        table("CPU (ms)", m_cpu, false);
        double gpuTotal = 0.0;
        for (const Stat& stat : m_gpu) {
            gpuTotal += stat.depth == 0 && stat.count > 0 ? stat.sum / stat.count : 0.0;
        }
        text += std::format("GPU (ms) : {:.3f} au total, en moyenne\n", gpuTotal);
        if (m_gpu.empty()) {
            text += "(heures du GPU absentes)\n\n";
        } else {
            table("Passes et étapes du GPU (ms)", m_gpu, false);
        }
        table("Scène (nombre)", m_counters, true);

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
        if (!file) {
            return std::unexpected(std::format("impossible d'écrire {}", Platform::ToGenericUtf8(path)));
        }
        return path;
    }
}
