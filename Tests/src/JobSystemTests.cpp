// Fils de travail (Core/JobSystem) : chaque élément d'un ParallelFor fait une fois et une seule, file pleine
// exécutée sur place, tâches qui lancent et attendent d'autres tâches (sans blocage), et JobSystem sans fil.
#include "Check.hpp"

#include "Flux/Core/JobSystem.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

using namespace Flux;

namespace {
    bool EachIndexOnce(JobSystem& jobs) {
        constexpr std::uint32_t Count = 100'000;
        std::vector<std::atomic<std::uint32_t>> seen(Count);
        jobs.ParallelFor(Count, 777, [&](std::uint32_t begin, std::uint32_t end) {
            for (std::uint32_t i = begin; i < end; ++i) {
                seen[i].fetch_add(1, std::memory_order_relaxed);
            }
        });
        for (const auto& value : seen) {
            CHECK(value.load() == 1);
        }
        // Moins d'éléments qu'une tranche, et aucun.
        std::atomic<std::uint32_t> small = 0;
        jobs.ParallelFor(5, 64, [&](std::uint32_t begin, std::uint32_t end) { small += end - begin; });
        CHECK(small == 5);
        jobs.ParallelFor(0, 64, [&](std::uint32_t, std::uint32_t) { small = 0; });
        CHECK(small == 5);
        return true;
    }

    // Plus de tâches que les files n'ont de cases : les dernières s'exécutent dans le fil qui les lance.
    bool ManyJobs(JobSystem& jobs) {
        constexpr std::uint32_t Count = 10'000;
        std::atomic<std::uint32_t> done = 0;
        JobCounter counter;
        for (std::uint32_t i = 0; i < Count; ++i) {
            jobs.Run(counter, [&done] { done.fetch_add(1, std::memory_order_relaxed); });
        }
        jobs.Wait(counter);
        CHECK(counter.IsDone());
        CHECK(done == Count);
        return true;
    }

    // Une tâche qui attend ses propres tâches aide au lieu de bloquer un fil : passe même avec un seul fil.
    bool NestedJobs(JobSystem& jobs) {
        std::atomic<std::uint32_t> total = 0;
        jobs.ParallelFor(64, 1, [&](std::uint32_t outerBegin, std::uint32_t outerEnd) {
            for (std::uint32_t i = outerBegin; i < outerEnd; ++i) {
                jobs.ParallelFor(1'000, 10, [&](std::uint32_t begin, std::uint32_t end) {
                    total.fetch_add(end - begin, std::memory_order_relaxed);
                });
            }
        });
        CHECK(total == 64'000);
        return true;
    }

    bool RunAll(unsigned workers) {
        JobSystem jobs(workers);
        CHECK(jobs.GetWorkerCount() == workers);
        CHECK(EachIndexOnce(jobs));
        CHECK(ManyJobs(jobs));
        CHECK(NestedJobs(jobs));
        return true;
    }
}

bool RunJobSystemTests() {
    CHECK(RunAll(0));
    CHECK(RunAll(1));
    CHECK(RunAll(JobSystem::DefaultWorkerCount() > 2 ? JobSystem::DefaultWorkerCount() : 3));
    // Créé et détruit sans servir : les fils endormis se réveillent pour s'arrêter.
    { JobSystem idle(4); }
    return true;
}
