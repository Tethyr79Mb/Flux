#include "Flux/Core/JobSystem.hpp"

#include <algorithm>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace Flux {
    namespace {
        // Cases par file : une image lance quelques centaines de tâches au plus ; au-delà, Submit exécute sur place.
        constexpr std::uint32_t QueueCapacity = 1024;
        // Tours à chercher du travail avant de dormir : un fil réveillé par le système met des dizaines de
        // microsecondes à repartir, de quoi perdre le gain sur des tâches courtes.
        constexpr int SpinCount = 64;
    }

    struct JobSystem::Impl {
        // Anneau protégé par un verrou : peu de tâches par image, chacune bien plus longue que le verrou. Une file
        // sans verrou (Chase-Lev) ne vaudra la peine que si les mesures montrent de la contention.
        struct Queue {
            std::mutex mutex;
            std::vector<Job> jobs = std::vector<Job>(QueueCapacity);
            std::uint32_t head = 0; // prochaine à prendre
            std::uint32_t size = 0;

            bool Push(const Job& job) {
                std::scoped_lock lock(mutex);
                if (size == QueueCapacity) {
                    return false;
                }
                jobs[(head + size) % QueueCapacity] = job;
                ++size;
                return true;
            }

            bool Pop(Job& job) {
                std::scoped_lock lock(mutex);
                if (size == 0) {
                    return false;
                }
                job = jobs[head];
                head = (head + 1) % QueueCapacity;
                --size;
                return true;
            }
        };

        // File 0 : celle des fils qui ne sont pas à ce JobSystem (fil principal) ; file i + 1 : fil de travail i.
        std::vector<Queue> queues;
        std::atomic<std::uint32_t> queued = 0; // tâches dans les files : les fils dorment dessus quand il vaut 0
        std::vector<std::jthread> workers;     // en dernier : arrêtés avant que les files disparaissent

        static thread_local const Impl* t_owner;
        static thread_local std::uint32_t t_queue;

        explicit Impl(unsigned workerCount) : queues(workerCount + 1) {}

        std::uint32_t OwnQueue() const { return t_owner == this ? t_queue : 0; }

        // Sa file d'abord, puis celles des autres en partant de la suivante, pour ne pas tous voler la même.
        bool Take(Job& job) {
            const std::uint32_t own = OwnQueue();
            const std::uint32_t count = static_cast<std::uint32_t>(queues.size());
            for (std::uint32_t i = 0; i < count; ++i) {
                if (queues[(own + i) % count].Pop(job)) {
                    queued.fetch_sub(1, std::memory_order_relaxed);
                    return true;
                }
            }
            return false;
        }

        static void Execute(Job& job) {
            job.invoke(job);
            JobCounter& counter = *job.counter;
            if (counter.m_pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                counter.m_pending.notify_all();
            }
        }

        void Work(std::stop_token stop, std::uint32_t queue) {
            t_owner = this;
            t_queue = queue;
            Job job;
            while (!stop.stop_requested()) {
                bool found = false;
                for (int spin = 0; spin < SpinCount && !found; ++spin) {
                    found = Take(job);
                    if (!found) {
                        std::this_thread::yield();
                    }
                }
                if (found) {
                    Execute(job);
                } else {
                    queued.wait(0, std::memory_order_relaxed);
                }
            }
        }
    };

    thread_local const JobSystem::Impl* JobSystem::Impl::t_owner = nullptr;
    thread_local std::uint32_t JobSystem::Impl::t_queue = 0;

    unsigned JobSystem::DefaultWorkerCount() {
        const unsigned cores = std::thread::hardware_concurrency();
        return cores > 1 ? cores - 1 : 0;
    }

    JobSystem::JobSystem(unsigned workerCount) : m_impl(std::make_unique<Impl>(workerCount)) {
        m_impl->workers.reserve(workerCount);
        for (unsigned i = 0; i < workerCount; ++i) {
            m_impl->workers.emplace_back([impl = m_impl.get(), i](std::stop_token stop) { impl->Work(stop, i + 1); });
        }
    }

    JobSystem::~JobSystem() {
        for (std::jthread& worker : m_impl->workers) {
            worker.request_stop();
        }
        // Réveille les fils endormis : ils voient l'arrêt demandé. Le compte n'a plus de sens ensuite.
        m_impl->queued.fetch_add(1, std::memory_order_relaxed);
        m_impl->queued.notify_all();
        m_impl->workers.clear();
    }

    unsigned JobSystem::GetWorkerCount() const {
        return static_cast<unsigned>(m_impl->workers.size());
    }

    void JobSystem::Submit(Job& job) {
        // Compté avant d'entrer dans la file : un fil qui la prend aussitôt ne fait jamais passer le compte sous zéro.
        if (!m_impl->workers.empty()) {
            m_impl->queued.fetch_add(1, std::memory_order_relaxed);
            if (m_impl->queues[m_impl->OwnQueue()].Push(job)) {
                m_impl->queued.notify_one();
                return;
            }
            m_impl->queued.fetch_sub(1, std::memory_order_relaxed);
        }
        Impl::Execute(job);
    }

    void JobSystem::Wait(JobCounter& counter) {
        Job job;
        int idle = 0;
        while (true) {
            const std::uint32_t pending = counter.m_pending.load(std::memory_order_acquire);
            if (pending == 0) {
                return;
            }
            if (m_impl->Take(job)) {
                Impl::Execute(job);
                idle = 0;
            } else if (++idle < SpinCount) {
                std::this_thread::yield();
            } else {
                // Plus rien dans les files : les tâches restantes tournent déjà sur d'autres fils.
                counter.m_pending.wait(pending, std::memory_order_acquire);
                idle = 0;
            }
        }
    }
}
