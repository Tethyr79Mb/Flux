#include "Renderer/ModelLoader.hpp"

#include <algorithm>
#include <iterator>

namespace Flux {
    namespace {
        // Deux fils : un gros modèle n'empêche pas les petits d'arriver, sans décoder trop d'images 4096 à la fois
        // (64 Mo chacune en mémoire).
        constexpr unsigned MaxThreads = 2;
    }

    ModelLoader::ModelLoader() {
        const unsigned count = std::clamp(std::thread::hardware_concurrency() / 2, 1u, MaxThreads);
        for (unsigned i = 0; i < count; ++i) {
            m_threads.emplace_back([this](std::stop_token stop) { Run(stop); });
        }
    }

    void ModelLoader::Request(std::string key, std::filesystem::path path) {
        {
            const std::scoped_lock lock(m_mutex);
            m_queue.push_back({std::move(key), std::move(path)});
            ++m_pending;
        }
        m_wake.notify_one();
    }

    void ModelLoader::TakeFinished(std::vector<Result>& results) {
        const std::scoped_lock lock(m_mutex);
        if (m_finished.empty()) {
            return;
        }
        m_pending -= m_finished.size();
        results.insert(results.end(), std::make_move_iterator(m_finished.begin()), std::make_move_iterator(m_finished.end()));
        m_finished.clear();
    }

    std::size_t ModelLoader::GetPending() const {
        const std::scoped_lock lock(m_mutex);
        return m_pending;
    }

    void ModelLoader::Run(std::stop_token stop) {
        while (true) {
            Job job;
            {
                std::unique_lock lock(m_mutex);
                if (!m_wake.wait(lock, stop, [this] { return !m_queue.empty(); })) {
                    return; // arrêt demandé
                }
                job = std::move(m_queue.front());
                m_queue.pop_front();
            }
            auto data = LoadModelFile(job.path);
            const std::scoped_lock lock(m_mutex);
            m_finished.push_back({std::move(job.key), std::move(data)});
        }
    }
}
