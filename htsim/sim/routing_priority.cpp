#include "routing_priority.h"
#include "online_fct_predictor.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace {
using G = GraStaticGraph;
using Key = G::EventKey;
using Flow = G::FlowKey;

// Nominal event timing for the static Global Slack and Join Slack strategies.
class EventTimeEstimator {
public:
    struct Prediction {
        std::set<uint64_t> dags;
        std::set<Key> frontier;
        std::map<uint64_t, size_t> frontier_counts;
        std::map<Key, long double> eta;
    };
    explicit EventTimeEstimator(const G& graph) : _graph(graph) {
        for (const auto& arc : graph.eventArcs()) {
            inputs[arc.target].push_back(arc);
            outputs[arc.source].push_back(arc);
        }
        for (const auto& dag : graph.dags())
            for (size_t node = 0; node < dag.second.nodes.size(); ++node)
                if (dag.second.nodes[node].type == G::Type::COMM)
                    finishes.emplace(Flow{dag.first, dag.second.nodes[node].flow_id},
                                     Key{dag.first, node, G::Event::FINISH});
        for (auto it = graph.eventOrder().rbegin(); it != graph.eventOrder().rend(); ++it) {
            auto& tail = nominal_tail[*it];
            const auto outgoing = outputs.find(*it);
            if (outgoing == outputs.end()) terminals[it->dag_index].push_back(*it);
            else for (const auto& arc : outgoing->second)
                // No stopping at Join entries. This is the complete nominal
                // tail from this event to a final workload terminal.
                tail = std::max(tail, arc.cost_ns + nominal_tail.at(arc.target));
        }
    }
    Prediction predict(const GraExecutionProgress& progress, uint64_t now_ns,
        const std::map<MessageletKey, RoutingDemandPolicy::Ready>& pending) const {
        Prediction result;
        for (const auto& item : pending) result.dags.insert(item.first.dag_id);
        for (auto dag : result.dags) {
            const auto visible = progress.frontier(dag);
            result.frontier.insert(visible.begin(), visible.end());
            result.frontier_counts[dag] = visible.size();
        }
        // Confirmed events retain their actual times. Frontier starts run now;
        // blocked starts wait for inputs. START -> FINISH costs subtract elapsed
        // service for active work. Overdue unconfirmed FINISH keeps one ns.
        const long double now = now_ns;
        for (const auto& event : _graph.eventOrder()) {
            if (!result.dags.count(event.dag_index)) continue;
            const auto actual = progress.eventTime(event);
            if (actual) { result.eta[event] = *actual; continue; }
            long double arrival = now;
            const auto incoming = inputs.find(event);
            if (incoming != inputs.end() &&
                (event.event == G::Event::FINISH || !result.frontier.count(event)))
                for (const auto& arc : incoming->second)
                    arrival = std::max(arrival, result.eta.at(arc.source) + arc.cost_ns);
            if (event.event == G::Event::FINISH) arrival = std::max(arrival, now + 1);
            result.eta[event] = arrival;
        }
        return result;
    }
    std::map<Key, std::vector<G::EventArc>> inputs, outputs;
    std::map<Flow, Key> finishes;
    std::map<uint64_t, std::vector<Key>> terminals;
    std::map<Key, long double> nominal_tail;
private:
    const G& _graph;
};

void rankEstimates(RoutingPriorityStrategy::Estimates& estimates) {
    // Reuse integer-priority arbitration. Equal relative slack gets the same
    // ordinal rank; no D or duration tie break is introduced.
    std::vector<std::pair<long double, MessageletKey>> ordered;
    for (const auto& item : estimates) if (item.second.scope != "completed")
        ordered.push_back({item.second.relative_slack.value_or(0), item.first});
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first; // least urgent first
        return a.second < b.second;
    });
    uint64_t rank = 0;
    std::optional<long double> previous;
    for (const auto& item : ordered) {
        if (!previous || *previous != item.first) ++rank;
        estimates.at(item.second).priority = rank;
        previous = item.first;
    }
}

class DownstreamPriority final : public RoutingPriorityStrategy {
public:
    explicit DownstreamPriority(const G& graph) : _graph(graph) {}
    Estimates evaluate(const GraExecutionProgress&, uint64_t,
        const std::map<MessageletKey, RoutingDemandPolicy::Ready>& pending) const override {
        Estimates result;
        for (const auto& item : pending) {
            Estimate estimate;
            estimate.priority = _graph.dTable().at({item.first.dag_id, item.first.flow_id});
            estimate.strategy = "dag_downstream";
            estimate.scope = "static";
            result.emplace(item.first, estimate);
        }
        return result;
    }
private:
    const G& _graph;
};

class JoinSlackPriority final : public RoutingPriorityStrategy {
public:
    bool usesProgress() const override { return true; }
    explicit JoinSlackPriority(const G& graph) : _graph(graph), _timing(graph) {
        // Terminal paths stop at the first physical Join, just as structureTable
        // does. A bypass that ends without a Join still gets a virtual context.
        std::map<Key, std::set<Key>> terminals;
        for (auto it = graph.eventOrder().rbegin(); it != graph.eventOrder().rend(); ++it) {
            const auto outgoing = _timing.outputs.find(*it);
            if (outgoing == _timing.outputs.end()) {
                terminals[*it].insert(*it);
                _terminals[it->dag_index].push_back(*it);
            } else for (const auto& arc : outgoing->second) {
                if (arc.join_entry) continue;
                const auto& downstream = terminals[arc.target];
                terminals[*it].insert(downstream.begin(), downstream.end());
            }
        }
        for (const auto& dag : graph.dags()) {
            for (size_t node = 0; node < dag.second.nodes.size(); ++node) {
                const auto& value = dag.second.nodes[node];
                if (value.type != G::Type::COMM) continue;
                const Flow flow{dag.first, value.flow_id};
                const Key finish{dag.first, node, G::Event::FINISH};
                auto& contexts = _contexts[flow];
                for (const auto& record : graph.structureTable().at(flow))
                    contexts.push_back({record.join_id, record.branch_id, std::nullopt});
                for (const auto& terminal : terminals[finish])
                    contexts.push_back({std::nullopt, terminal.node_index, terminal});
            }
        }
    }

    Estimates evaluate(const GraExecutionProgress& progress, uint64_t now_ns,
        const std::map<MessageletKey, RoutingDemandPolicy::Ready>& pending) const override {
        Estimates result;
        if (pending.empty()) return result;
        const auto prediction = _timing.predict(progress, now_ns, pending);
        const auto& dags = prediction.dags;
        const auto& frontier = prediction.frontier;
        const auto& frontier_counts = prediction.frontier_counts;
        const auto& eta = prediction.eta;
        const long double now = now_ns;
        const auto remaining = [&](Key event) { return std::max(0.0L, eta.at(event) - now); };
        // Include every branch of the Join, even remote, blocked, compute-only,
        // or already-committed branches absent from the local pending set.
        std::map<G::JoinKey, std::map<uint64_t, long double>> joins;
        std::map<G::JoinKey, bool> open;
        for (const auto& item : _graph.joinBranches()) {
            const auto dag = std::get<0>(item.first);
            if (!dags.count(dag)) continue;
            const G::JoinKey join{dag, std::get<1>(item.first)};
            auto& value = joins[join][std::get<2>(item.first)];
            for (const auto& event : item.second) {
                value = std::max(value, remaining(event));
                if (!progress.eventTime(event)) open[join] = true;
            }
        }
        for (const auto& item : pending) {
            const auto& key = item.first;
            const Flow flow{key.dag_id, key.flow_id};
            const auto finish = _timing.finishes.at(flow);
            Estimate estimate;
            estimate.strategy = "join_slack";
            estimate.scope = "no_open_join";
            estimate.frontier_events = frontier_counts.at(key.dag_id);
            estimate.finish_at_frontier = frontier.count(finish) != 0;
            if (progress.eventTime(finish)) {
                estimate.scope = "completed";
                result.emplace(key, estimate);
                continue;
            }
            for (const auto& context : _contexts.at(flow)) {
                long double own = 0, peer = 0;
                size_t peers = 0;
                if (context.join) {
                    const G::JoinKey join{key.dag_id, *context.join};
                    if (!open[join]) continue;
                    const auto& branches = joins.at(join);
                    own = branches.at(context.branch);
                    for (const auto& branch : branches) if (branch.first != context.branch) {
                        peer = std::max(peer, branch.second);
                        ++peers;
                    }
                } else {
                    own = remaining(*context.terminal);
                    for (const auto& terminal : _terminals.at(key.dag_id)) if (!(terminal == *context.terminal)) {
                        peer = std::max(peer, remaining(terminal));
                        ++peers;
                    }
                }
                const auto slack = peers ? peer - own : 0.0L;
                const auto relative = slack / std::max({own, peer, 1.0L});
                // Fanout can affect several nearest Joins: protect its most
                // limiting context. No downstream D is used to break ties.
                if (!estimate.relative_slack || relative < *estimate.relative_slack) {
                    estimate.scope = context.join ? "join" : "virtual_join";
                    estimate.join_id = context.join;
                    estimate.branch_id = context.branch;
                    estimate.remaining_ns = own;
                    estimate.peer_remaining_ns = peers ? peer : own;
                    estimate.slack_ns = slack;
                    estimate.relative_slack = relative;
                }
            }
            result.emplace(key, estimate);
        }
        rankEstimates(result);
        return result;
    }
private:
    struct Context {
        std::optional<uint64_t> join;
        uint64_t branch;
        std::optional<Key> terminal;
    };
    const G& _graph;
    EventTimeEstimator _timing;
    std::map<Flow, std::vector<Context>> _contexts;
    std::map<uint64_t, std::vector<Key>> _terminals;
};

void setGlobalSlack(RoutingPriorityStrategy::Estimate& estimate, uint64_t now_ns, long double finish,
    long double nominal_tail, long double tail, long double end, size_t end_node) {
    const auto slack = end - (finish + tail);
    estimate.estimated_finish_time_ns = finish;
    estimate.nominal_tail_ns = nominal_tail;
    estimate.estimated_workload_end_ns = end;
    estimate.global_end_node_index = end_node;
    estimate.global_slack_ns = slack;
    estimate.slack_ns = slack;
    estimate.remaining_ns = std::max(0.0L, finish - now_ns);
    // The denominator is identical within one workload. Across DAGs it
    // avoids raw-time bias.
    estimate.relative_slack = slack / std::max(1.0L, end - now_ns);
}

class GlobalSlackPriority final : public RoutingPriorityStrategy {
public:
    explicit GlobalSlackPriority(const G& graph) : _timing(graph) {}
    bool usesProgress() const override { return true; }
    Estimates evaluate(const GraExecutionProgress& progress, uint64_t now_ns,
        const std::map<MessageletKey, RoutingDemandPolicy::Ready>& pending) const override {
        Estimates result;
        if (pending.empty()) return result;
        const auto prediction = _timing.predict(progress, now_ns, pending);
        std::map<uint64_t, std::pair<long double, Key>> ends;
        for (auto dag : prediction.dags) {
            const auto& terminals = _timing.terminals.at(dag);
            auto latest = terminals.front();
            for (const auto& terminal : terminals)
                if (prediction.eta.at(terminal) > prediction.eta.at(latest)) latest = terminal;
            ends.emplace(dag, std::make_pair(prediction.eta.at(latest), latest));
        }
        for (const auto& item : pending) {
            const auto& key = item.first;
            const auto finish = _timing.finishes.at({key.dag_id, key.flow_id});
            Estimate estimate;
            estimate.strategy = "global_slack";
            estimate.scope = "global";
            estimate.frontier_events = prediction.frontier_counts.at(key.dag_id);
            estimate.finish_at_frontier = prediction.frontier.count(finish) != 0;
            if (progress.eventTime(finish)) {
                estimate.scope = "completed";
                result.emplace(key, estimate);
                continue;
            }
            const auto tail = _timing.nominal_tail.at(finish);
            const auto& end = ends.at(key.dag_id);
            setGlobalSlack(estimate, now_ns, prediction.eta.at(finish), tail, tail, end.first, end.second.node_index);
            result.emplace(key, estimate);
        }
        rankEstimates(result);
        return result;
    }
private:
    EventTimeEstimator _timing;
};

class OnlineGlobalSlackPriority final : public RoutingPriorityStrategy {
    static constexpr size_t no_flow = std::numeric_limits<size_t>::max();
    struct Arc { size_t event, flow; uint64_t nominal_ns; };
    struct Event { std::vector<Arc> inputs, outputs; bool finish = false; };
    struct Comm { size_t index, start, finish, predictor_index; };
    struct Component {
        std::vector<size_t> events, terminals, unconfirmed;
        std::vector<Comm> comms;
        std::optional<uint64_t> progress_version;
        uint64_t history_revision = 0, cost_time = 0, eta_time = 0;
        size_t frontier_count = 0, latest = 0;
        bool eta_time_initialized = false;
    };
    struct DagEstimate { size_t latest, frontier_count; };

public:
    OnlineGlobalSlackPriority(const G& graph, std::shared_ptr<const OnlineFctPredictor> predictor)
        : _graph(graph), _predictor(std::move(predictor)), _indexed_history(&_predictor->graph() == &graph),
          _events(graph.eventOrder().size()),
          _components(graph.componentCount()), _eta(_events.size()), _tail(_events.size()),
          _nominal_tail(_events.size()), _frontier(_events.size()), _actual(_events.size()) {
        std::vector<size_t> comm_at_start(_events.size(), no_flow);
        for (const auto& item : graph.dags()) {
            _dag_components.try_emplace(item.first);
            const auto& nodes = item.second.nodes;
            for (size_t node = 0; node < nodes.size(); ++node) {
                if (nodes[node].type != G::Type::COMM) continue;
                const Flow key{item.first, nodes[node].flow_id};
                const Comm comm{_comms.size(), graph.eventIndex({item.first, node, G::Event::START}),
                    graph.eventIndex({item.first, node, G::Event::FINISH}),
                    _indexed_history ? _predictor->flowIndex(key) : no_flow};
                _comms.emplace(key, comm);
                comm_at_start[comm.start] = comm.index;
                _components[graph.eventComponent(comm.start)].comms.push_back(comm);
            }
        }
        _fcts.resize(_comms.size());
        for (const auto& arc : graph.eventArcs()) {
            const auto src = graph.eventIndex(arc.source), dst = graph.eventIndex(arc.target);
            const bool lifecycle = arc.source.node_index == arc.target.node_index &&
                arc.source.event == G::Event::START && arc.target.event == G::Event::FINISH;
            const auto flow = lifecycle ? comm_at_start[src] : no_flow;
            _events[src].outputs.push_back({dst, flow, arc.cost_ns});
            _events[dst].inputs.push_back({src, flow, arc.cost_ns});
        }
        for (size_t i = 0; i < _events.size(); ++i) {
            const auto& key = graph.eventOrder()[i];
            const auto component = graph.eventComponent(i);
            auto& data = _components[component];
            if (data.events.empty()) _dag_components.at(key.dag_index).push_back(component);
            data.events.push_back(i);
            _events[i].finish = key.event == G::Event::FINISH;
            if (_events[i].outputs.empty()) data.terminals.push_back(i);
        }
        for (size_t i = _events.size(); i-- > 0;)
            for (const auto& arc : _events[i].outputs)
                _nominal_tail[i] = std::max(_nominal_tail[i], arc.nominal_ns + _nominal_tail[arc.event]);
    }

    bool usesProgress() const override { return true; }

    Estimates evaluate(const GraExecutionProgress& progress, uint64_t now_ns,
        const std::map<MessageletKey, RoutingDemandPolicy::Ready>& pending) const override {
        Estimates result;
        std::map<uint64_t, DagEstimate> dags;
        for (const auto& item : pending) {
            const auto& key = item.first;
            auto [dag, inserted] = dags.try_emplace(key.dag_id);
            if (inserted) dag->second = prepareDag(key.dag_id, progress, now_ns);
            const auto& comm = _comms.at({key.dag_id, key.flow_id});
            const auto& fct = _fcts[comm.index];
            Estimate estimate;
            estimate.strategy = "global_slack_online";
            estimate.scope = _actual[comm.finish] ? "completed" : "global";
            estimate.frontier_events = dag->second.frontier_count;
            estimate.finish_at_frontier = _frontier[comm.finish];
            estimate.collective_id = fct.collective_id;
            estimate.comm_position = fct.comm_position;
            estimate.prediction_time_ns = now_ns;
            estimate.predicted_fct_ns = fct.fct_ns;
            estimate.fct_sample_count = fct.sample_count;
            estimate.nominal_fallback = fct.nominal_fallback;
            estimate.fct_horizon = fct.horizon == OnlineFctPredictor::Horizon::ShortTerm ? "short_term" : "long_term";
            estimate.dynamic_tail_ns = _tail[comm.finish];
            if (!_actual[comm.finish]) {
                const auto end = dag->second.latest;
                setGlobalSlack(estimate, now_ns, _eta[comm.finish], _nominal_tail[comm.finish],
                    _tail[comm.finish], _eta[end], _graph.eventOrder()[end].node_index);
            }
            result.emplace(key, std::move(estimate));
        }
        rankEstimates(result);
        return result;
    }

private:
    long double cost(const Arc& arc) const {
        return arc.flow == no_flow ? arc.nominal_ns : _fcts[arc.flow].fct_ns;
    }

    bool later(size_t candidate, size_t other) const {
        // The reference selects the first equal terminal in reverse topo order.
        return _eta[candidate] > _eta[other] || (_eta[candidate] == _eta[other] && candidate > other);
    }

    DagEstimate prepareDag(uint64_t dag, const GraExecutionProgress& progress, uint64_t now_ns) const {
        DagEstimate result{_events.size(), 0};
        std::optional<std::set<Key>> external_frontier;
        if (&progress.graph() != &_graph) {
            const auto visible = progress.frontier(dag);
            external_frontier.emplace(visible.begin(), visible.end());
        }
        for (const auto id : _dag_components.at(dag)) {
            auto& component = _components[id];
            prepareComponent(id, component, progress, now_ns, external_frontier ? &*external_frontier : nullptr);
            result.frontier_count += component.frontier_count;
            if (result.latest == _events.size() || later(component.latest, result.latest))
                result.latest = component.latest;
        }
        if (external_frontier) result.frontier_count = external_frontier->size();
        return result;
    }

    OnlineFctPredictor::Prediction predictFct(const Comm& comm, uint64_t now_ns) const {
        const auto horizon = _actual[comm.start] ? OnlineFctPredictor::Horizon::ShortTerm
                                                 : OnlineFctPredictor::Horizon::LongTerm;
        if (_indexed_history) return _predictor->predictIndexed(comm.predictor_index, now_ns, horizon);
        const auto& start = _graph.eventOrder()[comm.start];
        const auto& flow = _graph.dags().at(start.dag_index).nodes[start.node_index];
        return _predictor->predict({start.dag_index, flow.flow_id}, now_ns, horizon);
    }

    void prepareComponent(size_t id, Component& component, const GraExecutionProgress& progress, uint64_t now_ns,
        const std::set<Key>* external_frontier) const {
        const auto version = external_frontier ? std::nullopt : std::optional<uint64_t>{progress.componentVersion(id)};
        const bool progress_changed = !version || component.progress_version != version;
        if (progress_changed) {
            component.frontier_count = 0;
            component.unconfirmed.clear();
            for (const auto i : component.events) {
                _actual[i] = external_frontier ? progress.eventTime(_graph.eventOrder()[i]) : progress.indexedTime(i);
                if (_actual[i]) _eta[i] = *_actual[i];
                else component.unconfirmed.push_back(i);
            }
            for (const auto i : component.events) {
                const auto& incoming = _events[i].inputs;
                _frontier[i] = external_frontier ? external_frontier->count(_graph.eventOrder()[i]) != 0
                    : !_actual[i] && std::all_of(incoming.begin(), incoming.end(),
                        [&](const Arc& arc) { return _actual[arc.event].has_value(); });
                component.frontier_count += _frontier[i];
            }
            component.progress_version = version;
        }

        const auto history = _indexed_history ? _predictor->componentHistory(id) : OnlineFctPredictor::ComponentHistory{};
        bool costs_changed = false;
        // Once every recorded sample in this component is visible, moving the
        // clock alone cannot change the predictions. A history update marks
        // every component that consumes the updated collective window.
        if (progress_changed || !_indexed_history || component.history_revision != history.revision ||
            (component.cost_time != now_ns && std::min(component.cost_time, now_ns) < history.latest_visible_ns)) {
            for (const auto& comm : component.comms) {
                const auto fct = predictFct(comm, now_ns);
                costs_changed |= _fcts[comm.index].fct_ns != fct.fct_ns;
                _fcts[comm.index] = fct;
            }
            component.history_revision = history.revision;
            component.cost_time = now_ns;
        }
        if (costs_changed || !component.eta_time_initialized) {
            // Reverse propagation is independent of both confirmed times and
            // the current clock. Only changes to an arc's cost invalidate it.
            for (auto it = component.events.rbegin(); it != component.events.rend(); ++it) {
                long double tail = 0;
                for (const auto& arc : _events[*it].outputs)
                    tail = std::max(tail, cost(arc) + _tail[arc.event]);
                _tail[*it] = tail;
            }
        }
        // Confirmed ETAs are fixed. Recompute only unconfirmed events in their
        // original forward order, and keep completed components across clocks.
        if (progress_changed || (!component.unconfirmed.empty() && (costs_changed || component.eta_time != now_ns))) {
            const long double now = now_ns;
            for (const auto i : component.unconfirmed) {
                const auto& event = _events[i];
                long double arrival = now;
                if (event.finish || !_frontier[i])
                    for (const auto& arc : event.inputs) arrival = std::max(arrival, _eta[arc.event] + cost(arc));
                if (event.finish) arrival = std::max(arrival, now + 1);
                _eta[i] = arrival;
            }
            component.latest = component.terminals.back();
            for (const auto terminal : component.terminals)
                if (later(terminal, component.latest)) component.latest = terminal;
            component.eta_time = now_ns;
            component.eta_time_initialized = true;
        }
    }

    const G& _graph;
    std::shared_ptr<const OnlineFctPredictor> _predictor;
    const bool _indexed_history;
    std::vector<Event> _events;
    std::map<Flow, Comm> _comms;
    std::map<uint64_t, std::vector<size_t>> _dag_components;
    mutable std::vector<Component> _components;
    mutable std::vector<long double> _eta, _tail;
    std::vector<long double> _nominal_tail;
    mutable std::vector<uint8_t> _frontier;
    mutable std::vector<std::optional<uint64_t>> _actual;
    mutable std::vector<OnlineFctPredictor::Prediction> _fcts;
};
}

std::unique_ptr<RoutingPriorityStrategy> makeRoutingPriorityStrategy(const std::string& strategy, const G& graph,
    std::shared_ptr<const OnlineFctPredictor> predictor) {
    if (strategy == "global_slack") return std::make_unique<GlobalSlackPriority>(graph);
    if (strategy == "global_slack_online") return std::make_unique<OnlineGlobalSlackPriority>(graph,
        predictor ? std::move(predictor) : std::make_shared<OnlineFctPredictor>(graph));
    if (strategy == "join_slack") return std::make_unique<JoinSlackPriority>(graph);
    if (strategy == "dag_downstream") return std::make_unique<DownstreamPriority>(graph);
    throw std::invalid_argument("Unsupported routing priority '" + strategy + "'");
}
