#include "gra_goal_graph.h"

#include <stdexcept>
#include <tuple>

#include "lgs/Parser.hpp"

GraGoalGraph buildGraGoalGraph(const Parser& parser, uint64_t dag_index,
                              const FlowRoutingConfig* identities) {
    using Task = std::pair<uint32_t, uint32_t>;
    using Message = std::tuple<uint32_t, uint32_t, uint32_t>; // src, dst, tag
    std::vector<std::vector<DeserializedNode>> ranks;
    std::map<Message, std::vector<Task>> receives;
    for (const auto& schedule : parser.schedules) ranks.push_back(schedule.GetAllNodes());
    for (size_t rank = 0; rank < ranks.size(); ++rank) {
        for (const auto& node : ranks[rank]) {
            if (node.Type == OPTYPE_RECV)
                receives[{node.Peer, static_cast<uint32_t>(rank), node.Tag}].push_back(
                    {static_cast<uint32_t>(rank), node.offset});
        }
    }

    GraGoalGraph result{{dag_index, {}}, {}};
    std::map<Task, size_t> logical;
    for (size_t rank = 0; rank < ranks.size(); ++rank) {
        for (const auto& node : ranks[rank]) {
            const Task task{static_cast<uint32_t>(rank), node.offset};
            if (node.Type == OPTYPE_RECV) continue;
            if (node.Type == OPTYPE_CALC) {
                logical.emplace(task, result.dag.nodes.size());
                result.dag.nodes.push_back({GraStaticGraph::Type::COMP, 0, node.Size, {}});
                continue;
            }
            if (node.Type != OPTYPE_SEND) throw std::invalid_argument("Unknown GOAL node type for GRA");
            FlowRoutingConfig::Entry entry{(uint64_t{rank} << 32) | node.offset,
                static_cast<uint32_t>(rank), node.Peer, node.Tag, node.offset, node.Size, "minimum"};
            if (identities) {
                const auto found = identities->sends.find(task);
                if (found == identities->sends.end())
                    throw std::invalid_argument("GRA flow identity missing for GOAL send task");
                entry = found->second;
                if (entry.src != rank || entry.send_task_id != node.offset || entry.dst != node.Peer ||
                    entry.tag != node.Tag || entry.bytes != node.Size)
                    throw std::invalid_argument("GRA flow identity does not match GOAL send task");
            }

            Task recv;
            if (entry.recv_task_id) {
                recv = {entry.dst, *entry.recv_task_id};
                if (recv.first >= ranks.size() || recv.second >= ranks[recv.first].size())
                    throw std::invalid_argument("GRA metadata receive task is outside GOAL graph");
            } else {
                const auto found = receives.find({entry.src, entry.dst, entry.tag});
                if (found == receives.end() || found->second.size() != 1)
                    throw std::invalid_argument("GRA requires an unambiguous SEND/RECV pair; supply recv_task metadata");
                recv = found->second.front();
                entry.recv_task_id = recv.second;
            }
            const auto& receiver = ranks[recv.first][recv.second];
            if (receiver.Type != OPTYPE_RECV || receiver.Peer != entry.src ||
                receiver.Tag != entry.tag || receiver.Size != entry.bytes)
                throw std::invalid_argument("GRA flow identity does not match GOAL receive task");
            const size_t index = result.dag.nodes.size();
            if (!logical.emplace(task, index).second || !logical.emplace(recv, index).second)
                throw std::invalid_argument("GRA communication endpoint belongs to multiple flows");
            result.dag.nodes.push_back({GraStaticGraph::Type::COMM, entry.flow_id, node.Size, {}});
            result.identities.sends.emplace(task, std::move(entry));
        }
    }
    if (identities && result.identities.sends.size() != identities->sends.size())
        throw std::invalid_argument("GRA metadata contains flows absent from GOAL graph");

    // Both finish and start dependencies are retained as nominal precedence
    // edges; runtime resource scheduling and observed timings are not used.
    for (size_t rank = 0; rank < ranks.size(); ++rank) {
        for (const auto& node : ranks[rank]) {
            const Task task{static_cast<uint32_t>(rank), node.offset};
            const auto found = logical.find(task);
            if (found == logical.end()) throw std::invalid_argument("Unmatched GOAL receive task for GRA");
            auto& successors = result.dag.nodes[found->second].successors;
            const auto add = [&](uint32_t target) {
                const auto next = logical.find({static_cast<uint32_t>(rank), target});
                if (next == logical.end()) throw std::invalid_argument("Invalid GOAL dependency for GRA");
                // An endpoint dependency within the same logical COMM is
                // internal, rather than a dependency on another graph node.
                if (next->second != found->second || target == node.offset)
                    successors.push_back(next->second);
            };
            for (auto target : node.DependOnMe) add(target);
            for (auto target : node.StartDependOnMe) add(target);
        }
    }
    return result;
}
