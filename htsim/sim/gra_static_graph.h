#ifndef GRA_STATIC_GRAPH_H
#define GRA_STATIC_GRAPH_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <optional>
#include <utility>
#include <vector>

// Startup-only global logical graphs and their nominal (not measured) costs.
// A COMM has one identity/cost, separate SEND/RECV starts, and a shared finish.
class GraStaticGraph {
public:
    enum class Type { COMM, COMP };
    // START means SEND issue for COMM, or task start for COMP. Matched FINISH
    // is shared by a COMM's endpoints; RECEIVE_START preserves RECV posting.
    enum class Event { START, RECEIVE_START, FINISH };
    struct Node {
        Type type;
        uint64_t flow_id; // used only by COMM
        uint64_t work; // COMM: payload bytes; COMP: estimated nanoseconds
        std::vector<size_t> successors; // shorthand for FINISH -> START dependencies
    };
    struct Dependency {
        size_t predecessor, successor;
        Event release = Event::FINISH;
        Event target = Event::START; // START or (for COMM) RECEIVE_START
    };
    using TaskKey = std::pair<uint32_t, uint32_t>; // original rank, task_offset
    struct TaskBinding {
        size_t node_index;
        Event start_event; // START for SEND/COMP; RECEIVE_START for RECV
    };
    using TaskIndex = std::map<TaskKey, TaskBinding>;
    struct EventKey {
        uint64_t dag_index;
        size_t node_index;
        Event event;
        bool operator<(const EventKey& other) const {
            return std::tie(dag_index, node_index, event) <
                std::tie(other.dag_index, other.node_index, other.event);
        }
        bool operator==(const EventKey& other) const {
            return std::tie(dag_index, node_index, event) ==
                std::tie(other.dag_index, other.node_index, other.event);
        }
    };
    using EventPredecessors = std::map<EventKey, std::vector<EventKey>>;
    using BranchKey = std::tuple<uint64_t, uint64_t, uint64_t>; // DAG, Join, branch
    struct EventArc {
        EventKey source, target;
        uint64_t cost_ns;
        std::optional<BranchKey> join_entry;
    };
    struct Dag {
        uint64_t dag_index;
        std::vector<Node> nodes;
        // The node projection may be cyclic. Only the expanded event graph
        // must be acyclic; all dependencies retain their event endpoints.
        std::vector<Dependency> dependencies = {};
        TaskIndex tasks = {}; // retained build-time lookup, not an analysis table
    };
    using FlowKey = std::pair<uint64_t, uint64_t>; // dag_index, DAG flow_id
    using JoinKey = std::pair<uint64_t, uint64_t>; // dag_index, join_id
    struct StructureRecord {
        uint64_t dag_index, flow_id, join_id, branch_id, tail_to_join_ns;
    };
    struct JoinRecord {
        uint64_t dag_index, join_id, branch_count;
    };
    // D is the longest weighted event path from SEND START. Structure tails
    // start at matched FINISH, stop before the first logical Join, and exclude
    // START-only branches already released independently of that FINISH.
    using DTable = std::map<FlowKey, uint64_t>;
    using StructureTable = std::map<FlowKey, std::vector<StructureRecord>>;
    using JoinTable = std::map<JoinKey, JoinRecord>;

    // reference_bandwidth_bps uses the topology's bits/s unit. Conversion to
    // ceil(bytes * 8 * 10^9 / bps) is exact, including fractional Byte/s.
    static std::shared_ptr<const GraStaticGraph> build(
        std::vector<Dag> dags, uint64_t reference_bandwidth_bps);

    const std::map<uint64_t, Dag>& dags() const { return _dags; }
    const DTable& dTable() const { return _d; }
    const StructureTable& structureTable() const { return _structure; }
    const JoinTable& joinTable() const { return _joins; }
    const EventPredecessors& eventPredecessors() const { return _event_predecessors; }
    const std::vector<EventKey>& eventOrder() const { return _event_order; }
    const std::vector<EventArc>& eventArcs() const { return _event_arcs; }
    const std::map<BranchKey, std::vector<EventKey>>& joinBranches() const { return _join_branches; }

private:
    GraStaticGraph(std::vector<Dag> dags, uint64_t reference_bandwidth_bps);
    std::map<uint64_t, Dag> _dags;
    DTable _d;
    StructureTable _structure;
    JoinTable _joins;
    EventPredecessors _event_predecessors;
    std::vector<EventKey> _event_order;
    std::vector<EventArc> _event_arcs;
    std::map<BranchKey, std::vector<EventKey>> _join_branches;
};

#endif
