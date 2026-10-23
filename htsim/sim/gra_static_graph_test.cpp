#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unistd.h>

#include "atlahs_dragonfly_api.h"
#include "datacenter/dragonfly_topology.h"
#include "gra_goal_graph.h"
#include "lgs/Parser.hpp"
#include "messagelet_runtime.h"
#include "uec_mp.h"

namespace {
using Type = GraStaticGraph::Type;
using Dag = GraStaticGraph::Dag;

template<class Exception, class Fn> void rejects(Fn fn) {
    bool caught = false;
    try { fn(); } catch (const Exception&) { caught = true; }
    assert(caught);
}

void checkTables() {
    Dag dag{0, {
        {Type::COMM, 10, 26, {1, 2, 9}},
        {Type::COMP, 0, 3, {3}},
        {Type::COMM, 20, 50, {3}},
        {Type::COMP, 0, 7, {4}},
        {Type::COMM, 30, 25, {7}},
        {Type::COMM, 40, 0, {6}},
        {Type::COMP, 0, 11, {7}},
        {Type::COMM, 50, 1, {8}},
        {Type::COMP, 0, 13, {}},
        {Type::COMP, 0, 5, {7}},
    }};
    auto graph = GraStaticGraph::build({dag, {7, {{Type::COMM, 10, 1, {}}}}}, 200000000000ULL);
    const auto& d = graph->dTable();
    assert(d.size() == 6);
    assert(d.at({0, 10}) == 27);
    assert(d.at({0, 20}) == 24);
    assert(d.at({0, 30}) == 15);
    assert(d.at({0, 40}) == 25);
    assert(d.at({0, 50}) == 14);
    assert(d.at({7, 10}) == 1);
    assert(graph->joinTable().size() == 2);
    assert(graph->joinTable().at({0, 0}).branch_count == 2);
    assert(graph->joinTable().at({0, 1}).branch_count == 3);
    const auto& structure = graph->structureTable();
    const auto& records = structure.at({0, 10});
    assert(records.size() == 3);
    assert(records[0].dag_index == 0 && records[0].flow_id == 10);
    assert(records[0].join_id == 0 && records[0].branch_id == 0 && records[0].tail_to_join_ns == 3);
    assert(records[1].join_id == 0 && records[1].branch_id == 1 && records[1].tail_to_join_ns == 2);
    // The nested join is visible only through the bypass, not through join 0.
    assert(records[2].join_id == 1 && records[2].branch_id == 2 && records[2].tail_to_join_ns == 5);
    assert(structure.at({0, 20}).size() == 1 && structure.at({0, 20})[0].tail_to_join_ns == 0);
    assert(structure.at({0, 30})[0].tail_to_join_ns == 0);
    assert(structure.at({0, 40})[0].tail_to_join_ns == 11);
    assert(structure.at({0, 50}).empty());
    assert(structure.at({7, 10}).empty());

    // Node IDs need not be in topological order. Duplicate endpoint edges
    // represent one logical dependency, rather than extra join branches.
    auto reordered = GraStaticGraph::build({{0, {
        {Type::COMP, 0, 5, {}}, {Type::COMM, 9, 1, {0, 0}},
    }}}, 3);
    assert(reordered->dTable().at({0, 9}) == 2666666672ULL);
    assert(reordered->joinTable().empty());
    assert(reordered->dags().at(0).nodes[1].successors.size() == 1);

    // A COMM that is itself a join still records its own downstream joins.
    auto comm_join = GraStaticGraph::build({{0, {
        {Type::COMM, 1, 25, {2}}, {Type::COMM, 2, 25, {2}},
        {Type::COMM, 3, 25, {3, 4}}, {Type::COMP, 0, 5, {5}},
        {Type::COMP, 0, 7, {5}}, {Type::COMP, 0, 11, {}},
    }}}, 200000000000ULL);
    assert(comm_join->dTable().at({0, 3}) == 19);
    const auto& onward = comm_join->structureTable().at({0, 3});
    assert(onward.size() == 2);
    assert(onward[0].join_id == 1 && onward[0].tail_to_join_ns == 5);
    assert(onward[1].join_id == 1 && onward[1].tail_to_join_ns == 7);
    assert(comm_join->structureTable().at({0, 1}).size() == 1);
    assert(comm_join->structureTable().at({0, 1})[0].join_id == 0);
}

void checkInvalidGraphs() {
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    rejects<std::invalid_argument>([] { GraStaticGraph::build({}, 0); });
    rejects<std::invalid_argument>([] {
        GraStaticGraph::build({{0, {}}, {0, {}}}, 8);
    });
    rejects<std::invalid_argument>([] {
        GraStaticGraph::build({{0, {{Type::COMM, 1, 0, {1}}}}}, 8);
    });
    rejects<std::invalid_argument>([] {
        GraStaticGraph::build({{0, {{Type::COMM, 1, 0, {1}}, {Type::COMP, 0, 0, {0}}}}}, 8);
    });
    rejects<std::invalid_argument>([] {
        GraStaticGraph::build({{0, {{Type::COMM, 1, 0, {}}, {Type::COMM, 1, 0, {}}}}}, 8);
    });
    rejects<std::overflow_error>([&] {
        GraStaticGraph::build({{0, {{Type::COMM, 1, max, {}}}}}, 1);
    });
    rejects<std::overflow_error>([&] {
        GraStaticGraph::build({{0, {{Type::COMM, 1, 1, {1}}, {Type::COMP, 0, max, {}}}}}, 8000000000ULL);
    });
    auto large = GraStaticGraph::build({{0, {{Type::COMM, 1, max, {}}}}}, 8000000000ULL);
    assert(large->dTable().at({0, 1}) == max);
    auto empty = GraStaticGraph::build({{0, {}}}, 8);
    assert(empty->dTable().empty() && empty->joinTable().empty() && empty->structureTable().empty());
}

// Minimal GOAL binary with dependencies on both communication endpoints:
// rank 0: SEND -> COMP(3); rank 1: COMP(7) -> RECV -> COMP(5).
// The two rank-local outgoing edges must survive as a COMM fanout.
template<class T> void write(std::ofstream& stream, T value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}
void node(std::ofstream& stream, uint32_t indegree, char type, uint32_t peer,
          uint64_t size, uint32_t outgoing, uint32_t appendix) {
    write(stream, indegree); write(stream, type); write(stream, peer);
    write(stream, size); write(stream, uint32_t{17});
    write(stream, uint8_t{0}); write(stream, uint8_t{0});
    write(stream, outgoing); write(stream, appendix);
    write(stream, uint32_t{0}); write(stream, uint32_t{0});
}

void checkGoalStartup() {
    char filename[] = "/tmp/htsim-gra-XXXXXX";
    const int fd = mkstemp(filename);
    assert(fd >= 0);
    close(fd);
    {
        std::ofstream out(filename, std::ios::binary);
        write(out, uint64_t{MAGIC_COOKIE});
        write(out, uint32_t{2}); write(out, uint8_t{0}); write(out, uint8_t{0});
        const uint64_t start = 4 + 1 + 1 + 16 * 2;
        const uint64_t rank0_size = 8 + 4 + 39 * 2 + 4;
        const uint64_t rank1_size = 8 + 4 + 39 * 3 + 8;
        write(out, start); write(out, start + rank0_size);
        write(out, start + rank0_size); write(out, start + rank0_size + rank1_size);
        write(out, uint32_t{2}); write(out, uint32_t{1}); write(out, uint32_t{0});
        node(out, 0, OPTYPE_SEND, 1, 26, 1, 0);
        node(out, 1, OPTYPE_CALC, 0, 3, 0, 1);
        write(out, uint32_t{1});
        write(out, uint32_t{3}); write(out, uint32_t{1}); write(out, uint32_t{0});
        node(out, 0, OPTYPE_CALC, 0, 7, 1, 0);
        node(out, 1, OPTYPE_RECV, 0, 26, 1, 1);
        node(out, 1, OPTYPE_CALC, 0, 5, 0, 2);
        write(out, uint32_t{1}); write(out, uint32_t{2});
        assert(out.good());
    }
    {
        Parser parser(filename, false);
        auto& events = EventList::getTheEventList();
        DragonflyTopology topology(COMPOSITE, 1000000, nullptr, &events);
        topology.construct_topology(2, 4, 2);
        AtlahsDragonflyApi api;
        api.setDragonflyTopology(&topology);
        rejects<std::logic_error>([&] { topology.groupRoutingAgent(0).staticGraph(); });
        api.initializeGoalGraph(parser);
        const auto& graph = topology.groupRoutingAgent(0).staticGraph();
        assert(graph.dTable().at({0, 0}) == 7); // ceil(26/25) + max(3,5)
        assert(graph.dags().at(0).nodes.size() == 4); // one COMM and three COMP
        assert(graph.dags().at(0).nodes[0].successors.size() == 2);
        assert(graph.dags().at(0).nodes[2].successors == std::vector<size_t>{0});
        for (uint32_t group = 0; group < topology.get_no_groups(); ++group)
            assert(&graph == &topology.groupRoutingAgent(group).staticGraph());
        rejects<std::logic_error>([&] { api.initializeGoalGraph(parser); });
        rejects<std::logic_error>([&] { topology.initializeGraStaticGraphs({}); });
        // Snapshotting has not consumed roots or modified dependency counts.
        assert(parser.schedules[1].GetAllNodes()[1].DependenciesCnt == 1);
        assert(parser.schedules[0].GetExecutableNodes_DSN().size() == 1);
        assert(parser.schedules[1].GetExecutableNodes_DSN().size() == 1);

        FlowRoutingConfig identities;
        identities.sends.emplace(std::make_pair(0, 0),
            FlowRoutingConfig::Entry{123, 0, 1, 17, 0, 26, "minimum", 1});
        auto with_ids = buildGraGoalGraph(parser, 7, &identities);
        auto with_tables = GraStaticGraph::build({with_ids.dag}, 200000000000ULL);
        assert(with_tables->dTable().at({7, 123}) == 7);
        assert(with_ids.identities.sends.at({0, 0}).flow_id == 123);
        identities.sends.at({0, 0}).recv_task_id = 0;
        rejects<std::invalid_argument>([&] { buildGraGoalGraph(parser, 0, &identities); });
        identities.sends.at({0, 0}).recv_task_id = 99;
        rejects<std::invalid_argument>([&] { buildGraGoalGraph(parser, 0, &identities); });
    }
    unlink(filename);
}
} // namespace

int main() {
    checkTables();
    checkInvalidGraphs();
    checkGoalStartup();
    std::cout << "GRA static graph tests passed\n";
}
