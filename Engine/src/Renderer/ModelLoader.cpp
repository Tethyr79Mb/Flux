#include "Renderer/ModelLoader.hpp"

#include "Flux/Core/Log.hpp"

#include <algorithm>
#include <iterator>

namespace Flux {
    namespace {
        // Deux fils : un gros modèle n'empêche pas les petits d'arriver, sans décoder trop d'images 4096 à la fois
        // (64 Mo chacune en mémoire).
        constexpr unsigned MaxThreads = 2;
        // Début de chaque image dans la mémoire d'envoi : optimalBufferCopyOffsetAlignment (au plus 256 sur PC).
        constexpr std::uint64_t ImageAlignment = 256;
    }

    ModelLoader::ModelLoader(RHI::Device& device) : m_device(device) {
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
            Result result{std::move(job.key), LoadModelFile(job.path), nullptr, {}};
            Stage(result);
            const std::scoped_lock lock(m_mutex);
            m_finished.push_back(std::move(result));
        }
    }

    void ModelLoader::Stage(Result& result) {
        if (!result.data || result.data->images.empty()) {
            return;
        }
        std::vector<ImageData>& images = result.data->images;
        result.imageOffsets.assign(images.size(), NoImage);
        std::uint64_t size = 0;
        for (std::size_t i = 0; i < images.size(); ++i) {
            const ImageData& image = images[i];
            const std::uint64_t bytes = std::uint64_t(std::max(image.size.x, 0)) * std::uint64_t(std::max(image.size.y, 0)) * 4;
            if (bytes > 0 && image.rgba.size() == bytes) {
                result.imageOffsets[i] = size;
                size = (size + bytes + ImageAlignment - 1) / ImageAlignment * ImageAlignment;
            }
        }
        if (size == 0) {
            result.imageOffsets.clear();
            return;
        }
        auto staging = m_device.CreateStagingBuffer(size);
        if (!staging) {
            // Les pixels restent dans les images : le fil de l'image les copiera (plus lent, mais affiché).
            Log::Warning("modèle : mémoire d'envoi de {} Mo refusée ({}), images copiées au chargement", size >> 20, staging.error());
            result.imageOffsets.clear();
            return;
        }
        for (std::size_t i = 0; i < images.size(); ++i) {
            if (result.imageOffsets[i] != NoImage) {
                (*staging)->Write(result.imageOffsets[i], images[i].rgba.data(), images[i].rgba.size());
                images[i].rgba = {}; // rendue tout de suite : une 4096 pèse 64 Mo
            }
        }
        result.staging = std::move(*staging);
    }
}
