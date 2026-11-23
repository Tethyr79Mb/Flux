#include "Flux/Animation/AnimationGraph.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <utility>

namespace Flux {
    namespace {
        using Json = nlohmann::json;
        using Graph = AnimationGraph;

        // Un message d'erreur, ou rien.
        using Result = std::expected<void, std::string>;

        Result ReadNumber(const Json& object, const char* key, float& value) {
            const auto found = object.find(key);
            if (found == object.end()) {
                return {};
            }
            if (!found->is_number()) {
                return std::unexpected(std::format("« {} » doit être un nombre", key));
            }
            value = found->get<float>();
            return {};
        }

        Result ReadBool(const Json& object, const char* key, bool& value) {
            const auto found = object.find(key);
            if (found == object.end()) {
                return {};
            }
            if (!found->is_boolean()) {
                return std::unexpected(std::format("« {} » doit être true ou false", key));
            }
            value = found->get<bool>();
            return {};
        }

        Result ReadText(const Json& object, const char* key, std::string& value) {
            const auto found = object.find(key);
            if (found == object.end()) {
                return {};
            }
            if (!found->is_string()) {
                return std::unexpected(std::format("« {} » doit être un texte", key));
            }
            value = found->get<std::string>();
            return {};
        }

        std::string_view Trim(std::string_view text) {
            const auto first = text.find_first_not_of(" \t");
            if (first == std::string_view::npos) {
                return {};
            }
            return text.substr(first, text.find_last_not_of(" \t") - first + 1);
        }

        // « paramètre opérateur nombre », par exemple « speed > 0.1 ». Les opérateurs de deux signes avant ceux d'un.
        std::expected<Graph::Condition, std::string> ParseCondition(const Graph& graph, std::string_view text) {
            static constexpr std::array<std::pair<std::string_view, Graph::Compare>, 6> Operators{{
                {"<=", Graph::Compare::LessEqual},
                {">=", Graph::Compare::GreaterEqual},
                {"==", Graph::Compare::Equal},
                {"!=", Graph::Compare::NotEqual},
                {"<", Graph::Compare::Less},
                {">", Graph::Compare::Greater},
            }};
            const auto invalid = [&] {
                return std::unexpected(std::format("condition « {} » : « paramètre opérateur nombre » attendu (opérateurs < <= > >= == !=)", text));
            };
            for (const auto& [symbol, compare] : Operators) {
                const auto at = text.find(symbol);
                if (at == std::string_view::npos) {
                    continue;
                }
                const std::string_view name = Trim(text.substr(0, at));
                const std::string_view number = Trim(text.substr(at + symbol.size()));
                Graph::Condition condition{.parameter = graph.FindParameter(name), .compare = compare};
                if (name.empty() || number.empty()) {
                    return invalid();
                }
                if (condition.parameter < 0) {
                    return std::unexpected(std::format("condition « {} » : paramètre « {} » non déclaré dans « parameters »", text, name));
                }
                const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), condition.value);
                if (error != std::errc{} || end != number.data() + number.size()) {
                    return invalid();
                }
                return condition;
            }
            return invalid();
        }

        Result ReadParameters(const Json& document, Graph& graph) {
            const auto found = document.find("parameters");
            if (found == document.end()) {
                return {};
            }
            if (!found->is_object()) {
                return std::unexpected("« parameters » doit être { \"nom\": valeur de départ, ... }");
            }
            if (found->size() > Graph::MaxParameters) {
                return std::unexpected(std::format("« parameters » : {} au plus", Graph::MaxParameters));
            }
            for (const auto& [name, value] : found->items()) {
                if (!value.is_number() && !value.is_boolean()) {
                    return std::unexpected(std::format("paramètre « {} » : un nombre (ou true, false) est attendu", name));
                }
                graph.parameters.push_back({name, value.is_boolean() ? (value.get<bool>() ? 1.0f : 0.0f) : value.get<float>()});
            }
            return {};
        }

        Result ReadState(const Json& entry, const Graph& graph, Graph::State& state) {
            if (!entry.is_object()) {
                return std::unexpected("un état est un objet { \"name\": ..., \"animation\": ... }");
            }
            if (auto read = ReadText(entry, "name", state.name); !read) {
                return read;
            }
            if (state.name.empty()) {
                return std::unexpected("un état sans « name »");
            }
            const auto context = [&state](const std::string& error) { return std::unexpected(std::format("état « {} » : {}", state.name, error)); };
            for (Result read : {ReadText(entry, "tag", state.tag), ReadNumber(entry, "speed", state.speed), ReadBool(entry, "loop", state.loop)}) {
                if (!read) {
                    return context(read.error());
                }
            }
            std::string single;
            if (auto read = ReadText(entry, "animation", single); !read) {
                return context(read.error());
            }
            const auto points = entry.find("points");
            if (!single.empty() && points != entry.end()) {
                return context("« animation » ou « points », pas les deux");
            }
            if (!single.empty()) {
                state.points.push_back({.animation = std::move(single)});
                return {};
            }
            if (points == entry.end() || !points->is_array() || points->empty()) {
                return context("« animation » (une seule) ou « points » ([{ \"animation\": ..., \"value\": ... }, ...]) attendu");
            }
            for (const Json& point : *points) {
                Graph::BlendPoint blend;
                if (!point.is_object()) {
                    return context("un point est { \"animation\": ..., \"value\": ... }");
                }
                for (Result read : {ReadText(point, "animation", blend.animation), ReadNumber(point, "value", blend.value),
                                    ReadNumber(point, "phase", blend.phase)}) {
                    if (!read) {
                        return context(read.error());
                    }
                }
                if (!state.points.empty() && blend.value <= state.points.back().value) {
                    return context("les « value » des points doivent croître");
                }
                state.points.push_back(std::move(blend));
            }
            std::string blendParameter;
            if (auto read = ReadText(entry, "blend", blendParameter); !read) {
                return context(read.error());
            }
            state.blendParameter = graph.FindParameter(blendParameter);
            if (state.blendParameter < 0) {
                return context(blendParameter.empty() ? "« blend » (le paramètre qui mélange les points) attendu"
                                                      : std::format("« blend » : paramètre « {} » non déclaré", blendParameter));
            }
            return {};
        }

        Result ReadTransition(const Json& entry, const Graph& graph, const Graph::StateMachine& machine, Graph::Transition& transition) {
            if (!entry.is_object()) {
                return std::unexpected("une transition est un objet { \"to\": ..., \"when\": [...] }");
            }
            std::string from;
            std::string to;
            for (Result read : {ReadText(entry, "from", from), ReadText(entry, "to", to), ReadNumber(entry, "duration", transition.duration),
                                ReadBool(entry, "atEnd", transition.atEnd)}) {
                if (!read) {
                    return std::unexpected(std::format("transition vers « {} » : {}", to, read.error()));
                }
            }
            const auto context = [&to](const std::string& error) { return std::unexpected(std::format("transition vers « {} » : {}", to, error)); };
            transition.to = machine.FindState(to);
            if (transition.to < 0) {
                return context(to.empty() ? "« to » attendu" : "pas d'état de ce nom");
            }
            transition.from = from.empty() || from == "*" ? -1 : machine.FindState(from);
            if (!from.empty() && from != "*" && transition.from < 0) {
                return context(std::format("« from » : pas d'état « {} »", from));
            }
            transition.duration = std::max(transition.duration, 0.0f);
            const auto when = entry.find("when");
            if (when == entry.end()) {
                return {};
            }
            if (!when->is_array()) {
                return context("« when » doit lister des conditions [\"speed > 0.1\", ...]");
            }
            for (const Json& text : *when) {
                if (!text.is_string()) {
                    return context("une condition est un texte, « speed > 0.1 »");
                }
                auto condition = ParseCondition(graph, text.get<std::string>());
                if (!condition) {
                    return context(condition.error());
                }
                transition.conditions.push_back(*condition);
            }
            return {};
        }
        // États, début et transitions d'une couche (à la racine du fichier pour le corps entier).
        Result ReadMachine(const Json& object, const Graph& graph, Graph::StateMachine& machine) {
            const auto states = object.find("states");
            if (states == object.end() || !states->is_array() || states->empty()) {
                return std::unexpected("« states » doit lister au moins un état");
            }
            for (const Json& entry : *states) {
                Graph::State state;
                if (auto read = ReadState(entry, graph, state); !read) {
                    return read;
                }
                if (machine.FindState(state.name) >= 0) {
                    return std::unexpected(std::format("deux états « {} »", state.name));
                }
                machine.states.push_back(std::move(state));
            }
            std::string start;
            if (auto read = ReadText(object, "start", start); !read) {
                return read;
            }
            machine.start = start.empty() ? 0 : machine.FindState(start);
            if (machine.start < 0) {
                return std::unexpected(std::format("« start » : pas d'état « {} »", start));
            }
            if (const auto transitions = object.find("transitions"); transitions != object.end()) {
                if (!transitions->is_array()) {
                    return std::unexpected("« transitions » doit être une liste");
                }
                for (const Json& entry : *transitions) {
                    Graph::Transition transition;
                    if (auto read = ReadTransition(entry, graph, machine, transition); !read) {
                        return read;
                    }
                    machine.transitions.push_back(std::move(transition));
                }
            }
            return {};
        }

        // Couche par-dessus le corps : { "name", "mask": "os", "weight": "paramètre" ou nombre, "fade", "modelSpace",
        // "states", ... }.
        Result ReadLayer(const Json& entry, const Graph& graph, Graph::Layer& layer) {
            if (!entry.is_object()) {
                return std::unexpected("une couche est un objet { \"name\": ..., \"mask\": ..., \"states\": [...] }");
            }
            for (Result read : {ReadText(entry, "name", layer.name), ReadText(entry, "mask", layer.mask), ReadNumber(entry, "fade", layer.fade),
                                 ReadBool(entry, "modelSpace", layer.modelSpace)}) {
                if (!read) {
                    return read;
                }
            }
            layer.fade = std::max(layer.fade, 0.0f);
            if (const auto weight = entry.find("weight"); weight != entry.end()) {
                if (weight->is_number()) {
                    layer.weight = std::clamp(weight->get<float>(), 0.0f, 1.0f);
                } else if (weight->is_string()) {
                    layer.weightParameter = graph.FindParameter(weight->get<std::string>());
                    if (layer.weightParameter < 0) {
                        return std::unexpected(std::format("« weight » : paramètre « {} » non déclaré", weight->get<std::string>()));
                    }
                } else {
                    return std::unexpected("« weight » doit être un nombre (0 à 1) ou le nom d'un paramètre");
                }
            }
            return ReadMachine(entry, graph, layer);
        }
    }

    int AnimationGraph::FindParameter(std::string_view name) const {
        const auto found = std::ranges::find(parameters, name, &Parameter::name);
        return found != parameters.end() ? static_cast<int>(found - parameters.begin()) : -1;
    }

    int AnimationGraph::StateMachine::FindState(std::string_view name) const {
        const auto found = std::ranges::find(states, name, &State::name);
        return found != states.end() ? static_cast<int>(found - states.begin()) : -1;
    }

    int AnimationGraph::StateMachine::FindTag(std::string_view tag) const {
        const auto found = std::ranges::find(states, tag, &State::tag);
        return !tag.empty() && found != states.end() ? static_cast<int>(found - states.begin()) : -1;
    }

    std::expected<AnimationGraph, std::string> ParseAnimationGraph(std::string_view text) {
        const Json document = Json::parse(text.begin(), text.end(), nullptr, false);
        if (document.is_discarded() || !document.is_object()) {
            return std::unexpected("JSON illisible (attendu : un objet { ... })");
        }
        AnimationGraph graph;
        if (const auto files = document.find("files"); files != document.end()) {
            if (!files->is_array() || !std::ranges::all_of(*files, [](const Json& file) { return file.is_string(); })) {
                return std::unexpected("« files » doit être [\"Animations/fichier.glb\", ...]");
            }
            for (const Json& file : *files) {
                graph.files.push_back(file.get<std::string>());
            }
        }
        if (auto read = ReadParameters(document, graph); !read) {
            return std::unexpected(read.error());
        }
        Graph::Layer body;
        body.name = "Corps";
        if (auto read = ReadMachine(document, graph, body); !read) {
            return std::unexpected(read.error());
        }
        graph.layers.push_back(std::move(body));
        if (const auto layers = document.find("layers"); layers != document.end()) {
            if (!layers->is_array() || layers->size() + 1 > Graph::MaxLayers) {
                return std::unexpected(std::format("« layers » doit lister au plus {} couches par-dessus le corps", Graph::MaxLayers - 1));
            }
            for (const Json& entry : *layers) {
                Graph::Layer layer;
                if (auto read = ReadLayer(entry, graph, layer); !read) {
                    return std::unexpected(std::format("couche « {} » : {}", layer.name, read.error()));
                }
                graph.layers.push_back(std::move(layer));
            }
        }
        return graph;
    }
}
