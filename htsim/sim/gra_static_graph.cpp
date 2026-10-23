#include "gra_static_graph.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

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
        std::vector<std::vector<size_t>> predecessors(count);
        std::vector<uint64_t> weights(count), downstream(count);
        for (size_t v = 0; v < count; ++v) {
            auto& node = dag.nodes[v];
            std::sort(node.successors.begin(), node.successors.end());
            node.successors.erase(std::unique(node.successors.begin(), node.successors.end()),
                                  node.successors.end());
            for (size_t u : node.successors) {
                if (u >= count) throw std::invalid_argument("Invalid GRA DAG successor");
                predecessors[u].push_back(v);
            }
            weights[v] = node.type == Type::COMM ? communicationCost(node.work, bps) : node.work;
        }

        // Deterministic IDs: node order for joins, predecessor order for branches.
        std::vector<uint64_t> joins(count);
        uint64_t next_join = 0;
        std::vector<size_t> pending(count), order;
        for (size_t v = 0; v < count; ++v) {
            pending[v] = predecessors[v].size();
            if (!pending[v]) order.push_back(v);
            if (pending[v] > 1) {
                joins[v] = next_join++;
                _joins.emplace(JoinKey{dag_index, joins[v]},
                               JoinRecord{dag_index, joins[v], pending[v]});
            }
        }
        for (size_t i = 0; i < order.size(); ++i) {
            for (size_t u : dag.nodes[order[i]].successors) {
                if (--pending[u] == 0) order.push_back(u);
            }
        }
        if (order.size() != count) throw std::invalid_argument("GRA logical graph contains a cycle");

        // frontier[v] contains only joins first encountered after v, with the
        // work strictly between v and the specified join input. Never propagate
        // a successor join's frontier across that join.
        using Frontier = std::map<std::pair<uint64_t, uint64_t>, uint64_t>;
        std::vector<Frontier> frontier(count);
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            const size_t v = *it;
            uint64_t longest = 0;
            for (size_t u : dag.nodes[v].successors) {
                longest = std::max(longest, downstream[u]);
                if (predecessors[u].size() > 1) {
                    const uint64_t branch = std::lower_bound(predecessors[u].begin(),
                        predecessors[u].end(), v) - predecessors[u].begin();
                    frontier[v].emplace(std::make_pair(joins[u], branch), 0);
                } else {
                    for (const auto& entry : frontier[u]) {
                        auto& tail = frontier[v][entry.first];
                        tail = std::max(tail, checkedAdd(weights[u], entry.second));
                    }
                }
            }
            downstream[v] = checkedAdd(weights[v], longest);
            const auto& node = dag.nodes[v];
            if (node.type != Type::COMM) continue;
            const FlowKey key{dag_index, node.flow_id};
            if (!_d.emplace(key, downstream[v]).second)
                throw std::invalid_argument("Duplicate COMM flow_id within GRA DAG");
            auto& records = _structure[key]; // terminal COMM has an empty set
            for (const auto& entry : frontier[v])
                records.push_back({dag_index, node.flow_id, entry.first.first,
                                   entry.first.second, entry.second});
        }
        _dags.emplace(dag_index, std::move(dag));
    }
}
