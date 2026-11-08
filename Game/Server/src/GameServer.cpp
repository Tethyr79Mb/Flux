#include "GameServer.hpp"

#include "GameProtocol.hpp"
#include "GameSim.hpp"

#include <Flux/Core/Log.hpp>
#include <Flux/Platform/Filesystem.hpp>

#include <chrono>
#include <format>
#include <thread>
#include <utility>

namespace Game {
    namespace {
        using Clock = std::chrono::steady_clock;
        using Seconds = std::chrono::duration<double>;

        constexpr Seconds ReportInterval{10.0};
    }

    GameServer::GameServer() : m_state(RegisterSimulation(m_simulation)) {}

    GameServer::~GameServer() {
        m_simulation.Stop();
        UnregisterSimulation(m_simulation);
    }

    std::expected<void, std::string> GameServer::Open(Flux::Project project, const std::filesystem::path& scene) {
        const std::filesystem::path path = scene.empty() ? project.GetStartupScenePath() : project.assetsDirectory / scene;
        m_simulation.SetProject(std::move(project));
        if (auto loaded = m_simulation.LoadScene(path); !loaded) {
            return std::unexpected(std::format("scène {} : {}", Flux::Platform::ToUtf8(path), loaded.error()));
        }
        m_simulation.Start();
        Flux::Log::Info("Scène {} chargée, {} pas par seconde", Flux::Platform::ToUtf8(path),
                        static_cast<int>(1.0 / Flux::SimulationClock::StepSeconds + 0.5));
        return {};
    }

    std::expected<void, std::string> GameServer::Listen(std::uint16_t port) {
        if (auto listening = m_network.Listen({.port = port, .gameVersion = GameProtocolVersion}); !listening) {
            return listening;
        }
        Flux::Log::Info("En attente des clients sur le port UDP {} (protocole du jeu {}, FluxNet {})", port,
                        GameProtocolVersion, Flux::Net::ProtocolVersion);
        return {};
    }

    void GameServer::ServeNetwork() {
        for (const Flux::Net::Event& event : m_network.Poll()) {
            switch (event.kind) {
                case Flux::Net::EventKind::Connected:
                    Flux::Log::Info("Client {} connecté ({} en tout)", event.connection, m_network.GetConnectionCount());
                    break;
                case Flux::Net::EventKind::Disconnected:
                    Flux::Log::Info("Client {} déconnecté : {} ({} en tout)", event.connection,
                                    Flux::Net::ToString(event.reason), m_network.GetConnectionCount());
                    break;
                case Flux::Net::EventKind::Message:
                    break; // aucun message du jeu avant l'étape 6
            }
        }
    }

    void GameServer::RunSteps(std::uint64_t count) {
        for (std::uint64_t i = 0; i < count; ++i) {
            m_simulation.Step(Flux::SimulationClock::StepSeconds);
        }
    }

    void GameServer::Run(const std::function<bool()>& shouldStop) {
        Flux::SimulationClock& clock = m_simulation.GetClock();
        Clock::time_point previous = Clock::now();
        Clock::time_point nextReport = previous + std::chrono::duration_cast<Clock::duration>(ReportInterval);
        // Bilan : temps passé à simuler, et tours où l'horloge a dû abandonner du temps (serveur trop lent).
        Seconds busy{0.0};
        std::uint64_t reportSteps = 0;
        int lateTurns = 0;
        while (!shouldStop()) {
            const Clock::time_point now = Clock::now();
            const int steps = clock.Advance(Seconds(now - previous).count());
            previous = now;
            ServeNetwork();
            for (int i = 0; i < steps; ++i) {
                m_simulation.Step(Flux::SimulationClock::StepSeconds);
            }
            m_network.Flush();
            reportSteps += static_cast<std::uint64_t>(steps);
            lateTurns += steps == Flux::SimulationClock::MaxStepsPerFrame ? 1 : 0;
            busy += Clock::now() - now;

            if (now >= nextReport) {
                const double stepMilliseconds = reportSteps > 0 ? busy.count() * 1000.0 / static_cast<double>(reportSteps) : 0.0;
                Flux::Log::Info("Pas {} : {:.2f} ms par pas, occupé {:.0f} % du temps, {} client(s)", clock.GetStep(),
                                stepMilliseconds, busy.count() * 100.0 / ReportInterval.count(),
                                m_network.GetConnectionCount());
                if (lateTurns > 0) {
                    Flux::Log::Warning("Serveur en retard : {} rattrapages de {} pas en 10 s", lateTurns,
                                       Flux::SimulationClock::MaxStepsPerFrame);
                }
                nextReport = now + std::chrono::duration_cast<Clock::duration>(ReportInterval);
                busy = Seconds{0.0};
                reportSteps = 0;
                lateTurns = 0;
            }

            // Jusqu'au pas suivant : l'horloge garde la part déjà écoulée. Une attente trop longue ne perd rien,
            // le pas en retard est rattrapé au tour suivant.
            std::this_thread::sleep_for(Seconds(Flux::SimulationClock::StepSeconds * (1.0 - clock.GetAlpha())));
        }
    }
}
