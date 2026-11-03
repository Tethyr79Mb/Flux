#pragma once

#include "Flux/Export.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace Flux::Platform {
    struct HttpRequest
    {
        std::string method; // GET, POST...
        std::string path;   // « /mcp », sans le domaine
        std::string origin; // en-tête Origin (vide s'il manque) : une page web qui appelle le serveur en a un
        std::string body;
    };

    struct HttpResponse
    {
        int status = 200;
        std::string contentType = "application/json";
        std::string body;
    };

    // Petit serveur HTTP/1.1 qui n'écoute que 127.0.0.1 (rien n'arrive d'un autre ordinateur) : outils de
    // développement (serveur MCP de l'éditeur). Des fils réseau lisent les requêtes et les mettent en file ; le fil
    // principal les prend quand il veut (TakeRequests, une fois par image) et y répond quand il a la réponse, même
    // plusieurs images plus tard (Respond) : la scène n'est jamais touchée par un autre fil.
    class FLUX_API LocalHttpServer {
        public:
        // port 0 : un port libre choisi par le système (GetPort le donne).
        static std::expected<std::unique_ptr<LocalHttpServer>, std::string> Start(std::uint16_t port);

        ~LocalHttpServer();

        LocalHttpServer(const LocalHttpServer&) = delete;
        LocalHttpServer& operator=(const LocalHttpServer&) = delete;

        struct Request
        {
            std::uint64_t id;
            HttpRequest http;
        };
        // Ajoute à out les requêtes arrivées depuis le dernier appel. Sans requête, une lecture atomique seulement.
        void TakeRequests(std::vector<Request>& out);
        // Réponse à une requête prise par TakeRequests. Sans effet si le client est parti entre-temps (ou après
        // 5 minutes sans réponse : il a reçu une erreur 503).
        void Respond(std::uint64_t id, HttpResponse response);

        std::uint16_t GetPort() const;

        private:
        struct Impl;
        explicit LocalHttpServer(std::unique_ptr<Impl> impl);

        std::unique_ptr<Impl> m_impl;
    };
}
