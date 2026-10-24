#ifdef NDEBUG
#undef NDEBUG
#endif

#include <algorithm>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include "gra_execution_progress.h"
#include "atlahs_dragonfly_api.h"
#include "datacenter/dragonfly_topology.h"
#include "messagelet_runtime.h"
#include "uec_mp.h"

namespace {
using Graph = GraStaticGraph;
using Event = Graph::Event;
using Key = Graph::EventKey;
using Result = GraExecutionProgress::Result;
using TaskEvent = GraTaskEvent;

template<class Exception, class Fn> void rejects(Fn fn) {
    bool caught = false;
    try { fn(); } catch (const Exception&) { caught = true; }
    assert(caught);
}

Graph::Dag fixture(uint64_t dag_index = 0) {
    return {dag_index, {
        {Graph::Type::COMM, 10, 2500, {2}},
        {Graph::Type::COMP, 0, 100, {3}},
        {Graph::Type::COMP, 0, 200, {3}},
        {Graph::Type::COMP, 0, 0, {}},
    }, {{0, 1, Event::START}}, {
        {{0, 10}, {0, Event::START}}, {{1, 20}, {0, Event::RECEIVE_START}},
        {{0, 11}, {1, Event::START}}, {{1, 21}, {2, Event::START}},
        {{2, 30}, {3, Event::START}},
    }};
}

bool atFrontier(const GraExecutionProgress& progress, size_t node, Event event, uint64_t dag = 0) {
    const auto frontier = progress.frontier(dag);
    return std::find(frontier.begin(), frontier.end(), Key{dag, node, event}) != frontier.end();
}

void checkFrontier() {
    auto graph = Graph::build({fixture()}, 200000000000ULL);
    const auto d_before = graph->dTable();
    const auto joins_before = graph->joinTable().size();
    const auto structures_before = graph->structureTable().at({0, 10}).size();
    GraExecutionProgress progress(graph);
    assert(progress.frontier(0).size() == 2);
    assert(atFrontier(progress, 0, Event::START));
    assert(atFrontier(progress, 0, Event::RECEIVE_START));
    assert(!atFrontier(progress, 0, Event::FINISH));

    assert(progress.receive({0, 0, 10, TaskEvent::START, 1, 10}) == Result::APPLIED);
    assert(progress.eventTime({0, 0, Event::START}) == 10);
    assert(atFrontier(progress, 1, Event::START)); // START dependency satisfied
    assert(!atFrontier(progress, 2, Event::START)); // FINISH still unconfirmed
    assert(!atFrontier(progress, 1, Event::FINISH)); // readiness is not execution
    assert(!atFrontier(progress, 0, Event::FINISH)); // RECV not posted yet
    assert(progress.confirmedEvents().size() == 1);

    progress.receive({0, 1, 20, TaskEvent::START, 1, 11});
    assert(progress.eventTime({0, 0, Event::RECEIVE_START}) == 11);
    assert(atFrontier(progress, 0, Event::FINISH));
    assert(!progress.eventTime({0, 0, Event::FINISH})); // network completion is still needed
    progress.receive({0, 0, 10, TaskEvent::FINISH, 2, 50});
    assert(progress.eventTime({0, 0, Event::FINISH}) == 50);
    assert(atFrontier(progress, 2, Event::START));
    const auto confirmed = progress.confirmedEvents().size();
    progress.receive({0, 1, 20, TaskEvent::FINISH, 2, 50});
    assert(progress.confirmedEvents().size() == confirmed); // no second COMM FINISH
    assert(progress.lastVersion(1) == 2);

    progress.receive({0, 0, 11, TaskEvent::START, 3, 51});
    assert(atFrontier(progress, 1, Event::FINISH));
    progress.receive({0, 0, 11, TaskEvent::FINISH, 4, 151});
    assert(!atFrontier(progress, 3, Event::START)); // Join still waits for COMP 2
    progress.receive({0, 1, 21, TaskEvent::START, 3, 60});
    progress.receive({0, 1, 21, TaskEvent::FINISH, 4, 260});
    assert(atFrontier(progress, 3, Event::START));
    assert(!atFrontier(progress, 3, Event::FINISH));
    assert(!progress.eventTime({0, 3, Event::START}));
    progress.receive({0, 2, 30, TaskEvent::START, 1, 260});
    assert(atFrontier(progress, 3, Event::FINISH)); // even zero work needs a real FINISH
    progress.receive({0, 2, 30, TaskEvent::FINISH, 2, 261});
    assert(progress.frontier(0).empty());
    assert(progress.confirmedEvents().size() == 9);
    assert(graph->dTable() == d_before);
    assert(graph->joinTable().size() == joins_before);
    assert(graph->structureTable().at({0, 10}).size() == structures_before);
}

void checkVersionsAndVisibility() {
    auto graph = Graph::build({fixture(), fixture(7)}, 200000000000ULL);
    GraExecutionProgress visible(graph), remote(graph);
    const GraProgressReport first{0, 0, 10, TaskEvent::START, 1, 10};
    visible.receive(first);
    assert(visible.receive(first) == Result::DUPLICATE);
    assert(visible.lastVersion(0) == 1 && visible.confirmedEvents().size() == 1);
    // Rank 0's next START is missing. Do not process its later FINISH.
    const GraProgressReport later{0, 0, 11, TaskEvent::FINISH, 3, 120};
    assert(visible.receive(later) == Result::BUFFERED);
    assert(visible.receive(later) == Result::DUPLICATE);
    assert(visible.pendingReports(0) == 1 && visible.lastVersion(0) == 1);
    assert(!visible.eventTime({0, 1, Event::FINISH}));
    assert(!atFrontier(visible, 1, Event::FINISH));
    assert(visible.receive({0, 0, 11, TaskEvent::START, 2, 20}) == Result::APPLIED);
    assert(visible.pendingReports(0) == 0 && visible.lastVersion(0) == 3);
    assert(visible.eventTime({0, 1, Event::START}) == 20);
    assert(visible.eventTime({0, 1, Event::FINISH}) == 120);
    assert(remote.confirmedEvents().empty()); // no read of another GRA/global scheduler
    assert(atFrontier(remote, 0, Event::START));
    assert(!atFrontier(remote, 1, Event::START));

    // Version streams are per rank, not per DAG. DAG progress stays separate.
    visible.receive({7, 0, 10, TaskEvent::START, 4, 130});
    assert(visible.eventTime({7, 0, Event::START}) == 130);
    assert(!visible.eventTime({7, 1, Event::FINISH}));
    // A receive-side completion cannot manufacture a logical COMM completion.
    remote.receive({0, 1, 20, TaskEvent::FINISH, 1, 50});
    assert(remote.confirmedEvents().empty() && remote.lastVersion(1) == 1);
    rejects<std::invalid_argument>([&] { visible.receive({0, 0, 10, TaskEvent::START, 0, 0}); });
    rejects<std::out_of_range>([&] { visible.receive({99, 0, 10, TaskEvent::START, 5, 140}); });
    rejects<std::out_of_range>([&] { visible.receive({0, 0, 99, TaskEvent::START, 5, 140}); });
    rejects<std::invalid_argument>([&] { visible.receive({0, 0, 10, TaskEvent::START, 5, 999}); });
    assert(visible.lastVersion(0) == 4 && visible.pendingReports(0) == 0);
    visible.receive({0, 0, 10, TaskEvent::START, 5, 10}); // idempotent logical event, new report
    assert(visible.lastVersion(0) == 5 && visible.eventTime({0, 0, Event::START}) == 10);
}

void checkHostGroundTruth() {
    auto& events = EventList::getTheEventList();
    DragonflyTopology topology(COMPOSITE, 1000000, nullptr, &events);
    topology.construct_topology(2, 4, 2);
    topology.initializeGraStaticGraphs({fixture()});
    AtlahsDragonflyApi api;
    api.setDragonflyTopology(&topology);
    api.total_nodes = topology.get_no_hosts();
    api.setGoalRankMappingFromBinaryHeader(3, 1, 1);
    api.reportTaskProgress(0, 10, false, 10, 0);
    api.reportTaskProgress(1, 20, false, 11, 0);
    // The adapter and direct runtime ingress share the runtime's rank sequence.
    api.getMessageletRuntime().reportTaskProgress(0, 10, true, 50, 0);
    api.reportTaskProgress(1, 20, true, 50, 0);
    for (uint32_t group = 0; group < topology.get_no_groups(); ++group) {
        const auto& gra = topology.groupRoutingAgent(group);
        const auto& progress = gra.executionProgress();
        assert(progress.lastVersion(0) == 2 && progress.lastVersion(1) == 2);
        assert(progress.confirmedEvents().size() == 3);
        assert(progress.eventTime({0, 0, Event::FINISH}) == 50);
        assert(atFrontier(progress, 1, Event::START) && atFrontier(progress, 2, Event::START));
        assert(!progress.eventTime({0, 1, Event::START}));
    }
    assert(&topology.groupRoutingAgent(0).executionProgress() !=
           &topology.groupRoutingAgent(1).executionProgress()); // independent storage
}
} // namespace

int main() {
    checkFrontier();
    checkVersionsAndVisibility();
    checkHostGroundTruth();
    std::cout << "GRA execution progress tests passed\n";
}
