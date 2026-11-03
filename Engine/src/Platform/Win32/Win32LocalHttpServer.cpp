// winsock2.h avant tout ce qui inclut windows.h : sinon l'ancien winsock.h entre en conflit.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "Flux/Platform/LocalHttpServer.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <format>
#include <list>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace Flux::Platform {
    namespace {
        constexpr std::size_t MaxHeaderBytes = 64 * 1024;
        constexpr std::size_t MaxBodyBytes = 16 * 1024 * 1024;
        // Le fil principal peut être bloqué (boîte de dialogue ouverte, chargement) : on attend longtemps avant
        // d'abandonner la requête.
        constexpr auto ResponseTimeout = std::chrono::minutes(5);

        std::string_view ReasonPhrase(int status) {
            switch (status) {
                case 200: return "OK";
                case 202: return "Accepted";
                case 400: return "Bad Request";
                case 403: return "Forbidden";
                case 404: return "Not Found";
                case 405: return "Method Not Allowed";
                case 413: return "Payload Too Large";
                case 503: return "Service Unavailable";
                default: return "Unknown";
            }
        }

        bool EqualsNoCase(std::string_view a, std::string_view b) {
            return std::ranges::equal(a, b, [](char x, char y) {
                return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
            });
        }

        std::string_view Trim(std::string_view text) {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
                text.remove_prefix(1);
            }
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
                text.remove_suffix(1);
            }
            return text;
        }

        bool SendAll(SOCKET socket, std::string_view data) {
            while (!data.empty()) {
                const int sent = send(socket, data.data(), static_cast<int>(std::min<std::size_t>(data.size(), 1 << 20)), 0);
                if (sent <= 0) {
                    return false;
                }
                data.remove_prefix(static_cast<std::size_t>(sent));
            }
            return true;
        }
    }

    struct LocalHttpServer::Impl
    {
        struct Connection
        {
            SOCKET socket = INVALID_SOCKET;
            std::thread thread;
            std::atomic<bool> done = false;
        };

        SOCKET listener = INVALID_SOCKET;
        std::uint16_t port = 0;
        std::thread acceptThread;
        std::atomic<bool> stopping = false;

        std::mutex mutex; // tout ce qui suit
        std::condition_variable answered;
        std::list<Connection> connections;
        std::vector<Request> queue;
        std::atomic<bool> hasRequests = false;
        std::unordered_set<std::uint64_t> waiting; // requêtes dont le client attend encore la réponse
        std::unordered_map<std::uint64_t, HttpResponse> responses;
        std::uint64_t nextId = 1;

        ~Impl() {
            stopping = true;
            closesocket(listener); // débloque accept
            if (acceptThread.joinable()) {
                acceptThread.join();
            }
            {
                std::scoped_lock lock(mutex);
                for (Connection& connection : connections) {
                    shutdown(connection.socket, SD_BOTH); // débloque recv
                }
            }
            answered.notify_all();
            for (Connection& connection : connections) {
                connection.thread.join();
                closesocket(connection.socket);
            }
            WSACleanup();
        }

        void Accept() {
            while (!stopping) {
                const SOCKET client = accept(listener, nullptr, nullptr);
                if (client == INVALID_SOCKET) {
                    if (stopping) {
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                std::scoped_lock lock(mutex);
                // Les connexions terminées sont rangées ici : leur fil a fini, join ne bloque pas.
                std::erase_if(connections, [](Connection& connection) {
                    if (!connection.done) {
                        return false;
                    }
                    connection.thread.join();
                    closesocket(connection.socket);
                    return true;
                });
                Connection& connection = connections.emplace_back();
                connection.socket = client;
                connection.thread = std::thread([this, &connection] {
                    Serve(connection.socket);
                    connection.done = true;
                });
            }
        }

        // Une connexion : plusieurs requêtes à la suite (keep-alive), chacune attend sa réponse avant la suivante.
        void Serve(SOCKET socket) {
            std::string buffer;
            char chunk[16 * 1024];
            while (!stopping) {
                std::size_t headerEnd;
                while ((headerEnd = buffer.find("\r\n\r\n")) == std::string::npos) {
                    if (buffer.size() > MaxHeaderBytes) {
                        SendAll(socket, "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        return;
                    }
                    const int received = recv(socket, chunk, sizeof(chunk), 0);
                    if (received <= 0) {
                        return;
                    }
                    buffer.append(chunk, static_cast<std::size_t>(received));
                }

                HttpRequest request;
                std::size_t contentLength = 0;
                bool close = false;
                const std::string_view head(buffer.data(), headerEnd);
                std::size_t lineEnd = head.find("\r\n");
                const std::string_view requestLine = head.substr(0, lineEnd);
                const std::size_t methodEnd = requestLine.find(' ');
                const std::size_t pathEnd = requestLine.find(' ', methodEnd + 1);
                if (methodEnd == std::string_view::npos || pathEnd == std::string_view::npos) {
                    SendAll(socket, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    return;
                }
                request.method = requestLine.substr(0, methodEnd);
                request.path = requestLine.substr(methodEnd + 1, pathEnd - methodEnd - 1);
                if (const std::size_t query = request.path.find('?'); query != std::string::npos) {
                    request.path.resize(query);
                }
                while (lineEnd != std::string_view::npos && lineEnd < head.size()) {
                    const std::size_t start = lineEnd + 2;
                    lineEnd = head.find("\r\n", start);
                    const std::string_view line = head.substr(start, lineEnd == std::string_view::npos ? head.npos : lineEnd - start);
                    const std::size_t colon = line.find(':');
                    if (colon == std::string_view::npos) {
                        continue;
                    }
                    const std::string_view name = Trim(line.substr(0, colon));
                    const std::string_view value = Trim(line.substr(colon + 1));
                    if (EqualsNoCase(name, "Content-Length")) {
                        std::from_chars(value.data(), value.data() + value.size(), contentLength);
                    } else if (EqualsNoCase(name, "Origin")) {
                        request.origin = value;
                    } else if (EqualsNoCase(name, "Connection")) {
                        close = EqualsNoCase(value, "close");
                    }
                }
                if (contentLength > MaxBodyBytes) {
                    SendAll(socket, "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    return;
                }
                buffer.erase(0, headerEnd + 4);
                while (buffer.size() < contentLength) {
                    const int received = recv(socket, chunk, sizeof(chunk), 0);
                    if (received <= 0) {
                        return;
                    }
                    buffer.append(chunk, static_cast<std::size_t>(received));
                }
                request.body = buffer.substr(0, contentLength);
                buffer.erase(0, contentLength);

                std::optional<HttpResponse> response;
                {
                    std::unique_lock lock(mutex);
                    const std::uint64_t id = nextId++;
                    waiting.insert(id);
                    queue.push_back({id, std::move(request)});
                    hasRequests = true;
                    answered.wait_for(lock, ResponseTimeout, [&] { return stopping || responses.contains(id); });
                    waiting.erase(id);
                    if (const auto found = responses.find(id); found != responses.end()) {
                        response = std::move(found->second);
                        responses.erase(found);
                    }
                }
                if (!response) {
                    response = HttpResponse{.status = 503, .contentType = "text/plain", .body = "le moteur ne répond pas"};
                    close = true;
                }
                const std::string header = std::format("HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\n"
                                                       "Connection: {}\r\n\r\n",
                                                       response->status, ReasonPhrase(response->status),
                                                       response->contentType, response->body.size(),
                                                       close ? "close" : "keep-alive");
                if (!SendAll(socket, header) || !SendAll(socket, response->body) || close) {
                    shutdown(socket, SD_BOTH);
                    return;
                }
            }
        }
    };

    std::expected<std::unique_ptr<LocalHttpServer>, std::string> LocalHttpServer::Start(std::uint16_t port) {
        WSADATA data;
        if (const int error = WSAStartup(MAKEWORD(2, 2), &data); error != 0) {
            return std::unexpected(std::format("WSAStartup : erreur {}", error));
        }
        auto impl = std::make_unique<Impl>();
        impl->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (impl->listener == INVALID_SOCKET) {
            return std::unexpected(std::format("socket : erreur {}", WSAGetLastError()));
        }
        // Un second éditeur ouvert ne vole pas le port du premier : il échoue (et le dit).
        BOOL exclusive = TRUE;
        setsockopt(impl->listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(impl->listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
            return std::unexpected(std::format("port {} déjà pris (un autre éditeur ouvert ?), erreur {}", port, WSAGetLastError()));
        }
        if (listen(impl->listener, SOMAXCONN) == SOCKET_ERROR) {
            return std::unexpected(std::format("listen : erreur {}", WSAGetLastError()));
        }
        // Port 0 : choisi par le système (tests) ; on lit lequel.
        int length = sizeof(address);
        getsockname(impl->listener, reinterpret_cast<sockaddr*>(&address), &length);
        impl->port = ntohs(address.sin_port);
        impl->acceptThread = std::thread([raw = impl.get()] { raw->Accept(); });
        return std::unique_ptr<LocalHttpServer>(new LocalHttpServer(std::move(impl)));
    }

    LocalHttpServer::LocalHttpServer(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

    LocalHttpServer::~LocalHttpServer() = default;

    void LocalHttpServer::TakeRequests(std::vector<Request>& out) {
        if (!m_impl->hasRequests.load(std::memory_order_acquire)) {
            return;
        }
        std::scoped_lock lock(m_impl->mutex);
        for (Request& request : m_impl->queue) {
            out.push_back(std::move(request));
        }
        m_impl->queue.clear();
        m_impl->hasRequests = false;
    }

    void LocalHttpServer::Respond(std::uint64_t id, HttpResponse response) {
        {
            std::scoped_lock lock(m_impl->mutex);
            if (!m_impl->waiting.contains(id)) {
                return;
            }
            m_impl->responses[id] = std::move(response);
        }
        m_impl->answered.notify_all();
    }

    std::uint16_t LocalHttpServer::GetPort() const {
        return m_impl->port;
    }
}
