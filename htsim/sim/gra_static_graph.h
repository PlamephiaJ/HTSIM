#ifndef GRA_STATIC_GRAPH_H
#define GRA_STATIC_GRAPH_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

// Startup-only global logical graphs and their nominal (not measured) costs.
// SEND/RECV endpoints are represented by one COMM, regardless of messagelets.
class GraStaticGraph {
public:
    enum class Type { COMM, COMP };
    struct Node {
        Type type;
        uint64_t flow_id; // used only by COMM
        uint64_t work; // COMM: payload bytes; COMP: estimated nanoseconds
        std::vector<size_t> successors;
    };
    struct Dag {
        uint64_t dag_index;
        std::vector<Node> nodes;
    };
    using FlowKey = std::pair<uint64_t, uint64_t>; // dag_index, DAG flow_id
    using JoinKey = std::pair<uint64_t, uint64_t>; // dag_index, join_id
    struct StructureRecord {
        uint64_t dag_index, flow_id, join_id, branch_id, tail_to_join_ns;
    };
    struct JoinRecord {
        uint64_t dag_index, join_id, branch_count;
    };
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

private:
    GraStaticGraph(std::vector<Dag> dags, uint64_t reference_bandwidth_bps);
    std::map<uint64_t, Dag> _dags;
    DTable _d;
    StructureTable _structure;
    JoinTable _joins;
};

#endif
