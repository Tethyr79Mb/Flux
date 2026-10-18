#pragma once

#include "Flux/Assets/Model.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Flux {
    // Fichiers de modèles lus sur des fils à part (LoadModelFile : glTF, décodage des images, rangement pour le rendu),
    // le plus long d'un chargement : le fil de l'image n'a plus qu'à envoyer le résultat au GPU (ModelCache).
    class ModelLoader {
        public:
        struct Result
        {
            std::string key; // celui de la demande
            std::expected<ModelData, std::string> data;
        };

        ModelLoader();
        ~ModelLoader() = default; // les fils finissent le fichier en cours, puis s'arrêtent
        ModelLoader(const ModelLoader&) = delete;
        ModelLoader& operator=(const ModelLoader&) = delete;

        // Lire `path` ; `key` revient avec le résultat.
        void Request(std::string key, std::filesystem::path path);
        // Ajoute à `results` les fichiers lus depuis le dernier appel ; sans allocation s'il n'y en a pas.
        void TakeFinished(std::vector<Result>& results);
        // Demandés et pas encore rendus par TakeFinished.
        std::size_t GetPending() const;

        private:
        struct Job
        {
            std::string key;
            std::filesystem::path path;
        };

        void Run(std::stop_token stop);

        mutable std::mutex m_mutex;
        std::condition_variable_any m_wake;
        std::deque<Job> m_queue;
        std::vector<Result> m_finished;
        std::size_t m_pending = 0;
        std::vector<std::jthread> m_threads; // en dernier : arrêtés et attendus avant le reste
    };
}
