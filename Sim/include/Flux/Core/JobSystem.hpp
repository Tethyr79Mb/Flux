#pragma once

#include "Flux/SimExport.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace Flux {
    // Tâches en cours d'un lot : Run l'augmente, la fin de chaque tâche le baisse, JobSystem::Wait attend zéro.
    class JobCounter {
        public:
        bool IsDone() const { return m_pending.load(std::memory_order_acquire) == 0; }

        private:
        friend class JobSystem;
        std::atomic<std::uint32_t> m_pending = 0;
    };

    // Fils de travail partagés par tout le moteur pour le travail court de chaque image (enregistrement des commandes,
    // préparation de la scène) : une file par fil, un fil sans travail en vole aux autres, et qui attend un lot
    // (Wait) exécute des tâches au lieu de dormir. Aucune allocation par tâche : la tâche est copiée dans une case
    // fixe, d'où des captures petites et copiables (références, pointeurs, nombres). Le travail long (chargement de
    // fichiers, compression) ne va pas ici : il occuperait un fil dont l'image a besoin (ModelLoader a ses fils).
    class FLUX_SIM_API JobSystem {
        public:
        static constexpr std::size_t MaxCaptureSize = 48;

        // Cœurs moins un : le fil principal travaille aussi, pendant ses Wait.
        static unsigned DefaultWorkerCount();

        // 0 fil : chaque tâche s'exécute dans le fil qui la lance (machines à un cœur, tests).
        explicit JobSystem(unsigned workerCount = DefaultWorkerCount());
        // Les lots lancés doivent être attendus avant.
        ~JobSystem();

        JobSystem(const JobSystem&) = delete;
        JobSystem& operator=(const JobSystem&) = delete;

        unsigned GetWorkerCount() const;

        // Lance work() sur un fil libre. File pleine : exécutée tout de suite, dans ce fil.
        template <typename Work>
        void Run(JobCounter& counter, Work&& work) {
            using Stored = std::decay_t<Work>;
            static_assert(sizeof(Stored) <= MaxCaptureSize, "tâche trop grosse : capturer par référence");
            static_assert(alignof(Stored) <= alignof(std::max_align_t), "tâche trop alignée");
            static_assert(std::is_trivially_copyable_v<Stored> && std::is_trivially_destructible_v<Stored>,
                          "tâche copiée sans constructeur : capturer des références, pointeurs ou nombres");
            Job job;
            job.counter = &counter;
            job.invoke = [](Job& self) { (*std::launder(reinterpret_cast<Stored*>(self.storage)))(); };
            ::new (static_cast<void*>(job.storage)) Stored(std::forward<Work>(work));
            counter.m_pending.fetch_add(1, std::memory_order_relaxed);
            Submit(job);
        }

        // Appelle work(begin, end) sur des tranches de batch éléments couvrant [0, count), réparties sur les fils, et
        // rend la main quand toutes sont finies. La première tranche est faite par ce fil.
        template <typename Work>
        void ParallelFor(std::uint32_t count, std::uint32_t batch, const Work& work) {
            batch = batch == 0 ? 1 : batch;
            if (count <= batch || GetWorkerCount() == 0) {
                if (count > 0) {
                    work(0u, count);
                }
                return;
            }
            JobCounter counter;
            for (std::uint32_t begin = batch; begin < count; begin += batch) {
                const std::uint32_t end = count - begin > batch ? begin + batch : count;
                Run(counter, [&work, begin, end] { work(begin, end); });
            }
            work(0u, batch);
            Wait(counter);
        }

        // Exécute des tâches en attendant ; dort seulement quand il n'y a plus rien à prendre.
        void Wait(JobCounter& counter);

        private:
        struct Job {
            void (*invoke)(Job&) = nullptr;
            JobCounter* counter = nullptr;
            alignas(std::max_align_t) std::byte storage[MaxCaptureSize];
        };

        void Submit(Job& job);

        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
