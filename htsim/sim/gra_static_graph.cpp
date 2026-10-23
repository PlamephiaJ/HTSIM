#include "gra_static_graph.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <tuple>

namespace {
uint64_t checkedAdd(uint64_t a, uint64_t b) {
    if (b > std::numeric_limits<uint64_t>::max() - a)
        throw std::overflow_error("GRA nominal dependency workload exceeds uint64_t ns");
    return a + b;
}

uint64_t communicationCost(uint64_t bytes, uint64_t bps) {
    const __uint128_t numerator = __uint128_t{bytes} * 8 * 1000000000;
    const auto ns = numerator / bps + (numerator % bps != 0);
    if (ns > std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("GRA nominal communication cost exceeds uint64_t ns");
    return static_cast<uint64_t>(ns);
}

using Branch = std::pair<uint64_t, uint64_t>; // join_id, branch_id
struct Edge {
    size_t target;
    uint64_t cost;
    std::optional<Branch> join_entry;
};
struct Events { size_t start, finish, receive_start; };
} // namespace

std::shared_ptr<const GraStaticGraph> GraStaticGraph::build(
    std::vector<Dag> dags, uint64_t bps) {
    return std::shared_ptr<const GraStaticGraph>(new GraStaticGraph(std::move(dags), bps));
}

GraStaticGraph::GraStaticGraph(std::vector<Dag> dags, uint64_t bps) {
    if (!bps) throw std::invalid_argument("GRA reference bandwidth must be positive");
    for (auto& dag : dags) {
        const uint64_t dag_index = dag.dag_index;
        if (_dags.count(dag_index)) throw std::invalid_argument("Duplicate GRA DAG index");
        const size_t count = dag.nodes.size();
        auto dependencies = dag.dependencies;
        for (size_t v = 0; v < count; ++v) {
            auto& successors = dag.nodes[v].successors;
            std::sort(successors.begin(), successors.end());
            successors.erase(std::unique(successors.begin(), successors.end()), successors.end());
            for (size_t u : successors) dependencies.push_back({v, u});
        }
        const auto dependencyKey = [](const Dependency& d) {
            return std::make_tuple(d.predecessor, d.successor, d.release, d.target);
        };
        std::sort(dependencies.begin(), dependencies.end(), [&](const auto& a, const auto& b) {
            return dependencyKey(a) < dependencyKey(b);
        });
        dependencies.erase(std::unique(dependencies.begin(), dependencies.end(),
            [&](const auto& a, const auto& b) { return dependencyKey(a) == dependencyKey(b); }),
            dependencies.end());
        // Retain the complete typed dependency graph for inspection. Its node
        // projection may contain cycles even when the expanded event DAG is valid.
        dag.dependencies = dependencies;

        std::vector<Events> events;
        std::vector<std::vector<Edge>> edges;
        for (const auto& node : dag.nodes) {
            const size_t start = edges.size();
            const size_t finish = start + 1;
            const size_t receive = node.type == Type::COMM ? start + 2 : start;
            edges.resize(start + (node.type == Type::COMM ? 3 : 2));
            events.push_back({start, finish, receive});
            const auto cost = node.type == Type::COMM ? communicationCost(node.work, bps) : node.work;
            // Transfer starts at SEND issue, independently of RECV posting.
            // Matched completion waits for both transfer and posted RECV:
            // t_finish = max(t_send_start + cost, t_receive_start).
            edges[start].push_back({finish, cost, std::nullopt});
            if (node.type == Type::COMM)
                edges[receive].push_back({finish, 0, std::nullopt});
        }
        const auto eventIndex = [&](size_t node, Event event) -> size_t {
            if (node >= count) throw std::invalid_argument("Invalid GRA dependency node");
            switch (event) {
                case Event::START: return events[node].start;
                case Event::FINISH: return events[node].finish;
                case Event::RECEIVE_START:
                    if (dag.nodes[node].type == Type::COMM) return events[node].receive_start;
                    break;
            }
            throw std::invalid_argument("Invalid GRA dependency event");
        };

        // Joins belong to logical workload nodes, not lifecycle bookkeeping.
        // Count distinct logical predecessors across both endpoint starts;
        // multiple typed edges from one predecessor still form one branch.
        std::vector<std::vector<size_t>> predecessors(count);
        for (const auto& dep : dependencies) {
            eventIndex(dep.predecessor, dep.release);
            eventIndex(dep.successor, dep.target);
            if (dep.target == Event::FINISH)
                throw std::invalid_argument("GRA dependency must target a start event");
            if (dep.predecessor != dep.successor)
                predecessors[dep.successor].push_back(dep.predecessor);
        }
        std::vector<uint64_t> joins(count);
        uint64_t next_join = 0;
        for (size_t v = 0; v < count; ++v) {
            auto& inputs = predecessors[v];
            std::sort(inputs.begin(), inputs.end());
            inputs.erase(std::unique(inputs.begin(), inputs.end()), inputs.end());
            if (inputs.size() > 1) {
                joins[v] = next_join++;
                _joins.emplace(JoinKey{dag_index, joins[v]},
                    JoinRecord{dag_index, joins[v], inputs.size()});
            }
        }
        for (const auto& dep : dependencies) {
            std::optional<Branch> branch;
            const auto& inputs = predecessors[dep.successor];
            if (inputs.size() > 1 && dep.predecessor != dep.successor) {
                branch = Branch{joins[dep.successor], std::lower_bound(inputs.begin(), inputs.end(),
                    dep.predecessor) - inputs.begin()};
            }
            edges[eventIndex(dep.predecessor, dep.release)].push_back(
                {eventIndex(dep.successor, dep.target), 0, branch});
        }

        std::vector<size_t> pending(edges.size()), order;
        for (const auto& outgoing : edges)
            for (const auto& edge : outgoing) ++pending[edge.target];
        for (size_t v = 0; v < edges.size(); ++v)
            if (!pending[v]) order.push_back(v);
        for (size_t i = 0; i < order.size(); ++i)
            for (const auto& edge : edges[order[i]])
                if (--pending[edge.target] == 0) order.push_back(edge.target);
        if (order.size() != edges.size())
            throw std::invalid_argument("GRA event graph contains a dependency cycle");

        // Costs live on lifecycle edges. START dependencies bypass the source
        // duration, so concurrent branches take max rather than summing work.
        using Frontier = std::map<Branch, uint64_t>;
        std::vector<Frontier> frontier(edges.size());
        std::vector<uint64_t> downstream(edges.size());
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            const size_t v = *it;
            for (const auto& edge : edges[v]) {
                downstream[v] = std::max(downstream[v], checkedAdd(edge.cost, downstream[edge.target]));
                if (edge.join_entry) {
                    frontier[v].emplace(*edge.join_entry, 0);
                } else {
                    for (const auto& entry : frontier[edge.target]) {
                        auto& tail = frontier[v][entry.first];
                        tail = std::max(tail, checkedAdd(edge.cost, entry.second));
                    }
                }
            }
        }
        for (size_t v = 0; v < count; ++v) {
            const auto& node = dag.nodes[v];
            if (node.type != Type::COMM) continue;
            const FlowKey key{dag_index, node.flow_id};
            if (!_d.emplace(key, downstream[events[v].start]).second)
                throw std::invalid_argument("Duplicate COMM flow_id within GRA DAG");
            auto& records = _structure[key];
            // Only dependencies released after this COMM finishes belong to
            // its tail. Work released at either endpoint's START is not causal
            // downstream work of this completion event.
            for (const auto& entry : frontier[events[v].finish])
                records.push_back({dag_index, node.flow_id, entry.first.first,
                                   entry.first.second, entry.second});
        }
        _dags.emplace(dag_index, std::move(dag));
    }
}
