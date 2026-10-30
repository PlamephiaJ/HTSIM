#include "routing_priority.h"
#include "online_fct_predictor.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace {
using G = GraStaticGraph;
using Key = G::EventKey;
using Flow = G::FlowKey;

// Shared event semantics. An optional cost snapshot replaces only COMM's
// SEND START -> FINISH edge, identically in forward and backward propagation.
class EventTimeEstimator {
public:
    using CommCosts = std::map<Key, long double>; // keyed by SEND START
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
        const std::map<MessageletKey, RoutingDemandPolicy::Ready>& pending,
        const CommCosts* costs = nullptr) const {
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
                    arrival = std::max(arrival, result.eta.at(arc.source) + cost(arc, costs));
            if (event.event == G::Event::FINISH) arrival = std::max(arrival, now + 1);
            result.eta[event] = arrival;
        }
        return result;
    }
    std::map<Key, long double> longestTails(const CommCosts& costs, const std::set<uint64_t>& dags) const {
        std::map<Key, long double> tails;
        for (auto it = _graph.eventOrder().rbegin(); it != _graph.eventOrder().rend(); ++it) {
            if (!dags.count(it->dag_index)) continue;
            auto& tail = tails[*it];
            const auto outgoing = outputs.find(*it);
            if (outgoing != outputs.end()) for (const auto& arc : outgoing->second)
                tail = std::max(tail, cost(arc, &costs) + tails.at(arc.target));
        }
        return tails;
    }
    std::map<Key, std::vector<G::EventArc>> inputs, outputs;
    std::map<Flow, Key> finishes;
    std::map<uint64_t, std::vector<Key>> terminals;
    std::map<Key, long double> nominal_tail;
private:
    static long double cost(const G::EventArc& arc, const CommCosts* costs) {
        if (costs && arc.source.node_index == arc.target.node_index &&
            arc.source.event == G::Event::START && arc.target.event == G::Event::FINISH) {
            const auto found = costs->find(arc.source);
            if (found != costs->end()) return found->second;
        }
        return arc.cost_ns;
    }
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

class GlobalSlackPriority final : public RoutingPriorityStrategy {
public:
    explicit GlobalSlackPriority(const G& graph, std::shared_ptr<const OnlineFctPredictor> predictor = nullptr)
        : _timing(graph), _predictor(std::move(predictor)) {}
    bool usesProgress() const override { return true; }
    Estimates evaluate(const GraExecutionProgress& progress, uint64_t now_ns,
        const std::map<MessageletKey, RoutingDemandPolicy::Ready>& pending) const override {
        Estimates result;
        if (pending.empty()) return result;
        EventTimeEstimator::CommCosts costs;
        std::map<Flow, OnlineFctPredictor::Prediction> fcts;
        if (_predictor) {
            std::set<uint64_t> dags;
            for (const auto& item : pending) dags.insert(item.first.dag_id);
            // Include blocked/downstream flows, not just local READY demands.
            for (const auto& item : _timing.finishes) {
                if (!dags.count(item.first.first)) continue;
                const auto fct = _predictor->predict(item.first, now_ns);
                fcts.emplace(item.first, fct);
                costs.emplace(Key{item.second.dag_index, item.second.node_index, G::Event::START}, fct.fct_ns);
            }
        }
        const auto prediction = _timing.predict(progress, now_ns, pending, _predictor ? &costs : nullptr);
        const auto dynamic_tails = _predictor ? _timing.longestTails(costs, prediction.dags) : std::map<Key, long double>{};
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
            estimate.strategy = _predictor ? "global_slack_online" : "global_slack";
            estimate.scope = "global";
            estimate.frontier_events = prediction.frontier_counts.at(key.dag_id);
            estimate.finish_at_frontier = prediction.frontier.count(finish) != 0;
            if (_predictor) {
                const auto& fct = fcts.at({key.dag_id, key.flow_id});
                estimate.collective_id = fct.collective_id;
                estimate.prediction_time_ns = now_ns;
                estimate.predicted_fct_ns = fct.fct_ns;
                estimate.fct_sample_count = fct.sample_count;
                estimate.nominal_fallback = fct.nominal_fallback;
                estimate.dynamic_tail_ns = dynamic_tails.at(finish);
            }
            if (progress.eventTime(finish)) {
                estimate.scope = "completed";
                result.emplace(key, estimate);
                continue;
            }
            const auto finish_time = prediction.eta.at(finish);
            const auto tail = _predictor ? dynamic_tails.at(finish) : _timing.nominal_tail.at(finish);
            const auto end_time = ends.at(key.dag_id).first;
            const auto slack = end_time - (finish_time + tail);
            estimate.estimated_finish_time_ns = finish_time;
            estimate.nominal_tail_ns = _timing.nominal_tail.at(finish);
            estimate.estimated_workload_end_ns = end_time;
            estimate.global_end_node_index = ends.at(key.dag_id).second.node_index;
            estimate.global_slack_ns = slack;
            estimate.slack_ns = slack;
            estimate.remaining_ns = std::max(0.0L, finish_time - now_ns);
            // The denominator is identical within one workload, preserving
            // exact S_global ordering. Across DAGs it avoids raw-time bias.
            estimate.relative_slack = slack / std::max(1.0L, end_time - now_ns);
            result.emplace(key, estimate);
        }
        rankEstimates(result);
        return result;
    }
private:
    EventTimeEstimator _timing;
    std::shared_ptr<const OnlineFctPredictor> _predictor;
};
}

std::unique_ptr<RoutingPriorityStrategy> makeRoutingPriorityStrategy(const std::string& strategy, const G& graph,
    std::shared_ptr<const OnlineFctPredictor> predictor) {
    if (strategy == "global_slack") return std::make_unique<GlobalSlackPriority>(graph);
    if (strategy == "global_slack_online") return std::make_unique<GlobalSlackPriority>(graph,
        predictor ? std::move(predictor) : std::make_shared<OnlineFctPredictor>(graph));
    if (strategy == "join_slack") return std::make_unique<JoinSlackPriority>(graph);
    if (strategy == "dag_downstream") return std::make_unique<DownstreamPriority>(graph);
    throw std::invalid_argument("Unsupported routing priority '" + strategy + "'");
}
