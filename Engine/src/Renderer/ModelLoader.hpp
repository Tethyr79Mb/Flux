#pragma once

#include "Flux/Assets/Model.hpp"
#include "Flux/Renderer/RHI/RHI.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Flux {
    // Fichiers de modèles lus sur des fils à part (LoadModelFile : glTF, décodage des images, rangement pour le rendu),
    // le plus long d'un chargement, puis pixels des images copiés dans une mémoire d'envoi : le fil de l'image n'a plus
    // qu'à enregistrer les copies vers le GPU (ModelCache).
    class ModelLoader {
        public:
        struct Result
        {
            std::string key; // celui de la demande
            std::expected<ModelData, std::string> data;
            // Pixels des images à la suite (data->images[i].rgba alors vidé), à imageOffsets[i] ; NoImage : image
            // restée dans rgba (vide ou taille fausse). staging nul : toutes restées dans rgba (mémoire refusée).
            std::unique_ptr<RHI::StagingBuffer> staging;
            std::vector<std::uint64_t> imageOffsets;
            // Niveaux de mip rangés à imageOffsets[i] avec l'image (BuildMipChain : feuillage) ; 1 : niveau 0 seul, le
            // GPU calcule les autres.
            std::vector<std::uint32_t> imageLevels;
        };
        static constexpr std::uint64_t NoImage = UINT64_MAX;

        // Seuil d'alpha de chaque image de data.images servant de couleur de base à un matériau à seuil (feuillage), 0
        // pour les autres : leurs mips sont calculés par BuildMipChain pour garder la couverture.
        static std::vector<float> GetMaskedCutoffs(const ModelData& data);

        // device : de quoi créer les mémoires d'envoi, depuis les fils de lecture (doit vivre plus longtemps).
        explicit ModelLoader(RHI::Device& device);
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
        // Range les pixels des images dans une mémoire d'envoi (Result::staging).
        void Stage(Result& result);

        RHI::Device& m_device;
        mutable std::mutex m_mutex;
        std::condition_variable_any m_wake;
        std::deque<Job> m_queue;
        std::vector<Result> m_finished;
        std::size_t m_pending = 0;
        std::vector<std::jthread> m_threads; // en dernier : arrêtés et attendus avant le reste
    };
}
