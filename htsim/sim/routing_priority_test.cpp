#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cmath>
#include <iostream>
#include "routing_priority.h"

namespace {
using G = GraStaticGraph;
using Type = G::Type;
using Event = G::Event;
using Pending = std::map<MessageletKey, RoutingDemandPolicy::Ready>;
constexpr uint64_t bps = 8000000000ULL; // one payload byte/ns

void bind(G::Dag& dag) {
    for (size_t i = 0; i < dag.nodes.size(); ++i) {
        dag.tasks.emplace(G::TaskKey{static_cast<uint32_t>(i), 0}, G::TaskBinding{i, Event::START});
        if (dag.nodes[i].type == Type::COMM)
            dag.tasks.emplace(G::TaskKey{static_cast<uint32_t>(100 + i), 0}, G::TaskBinding{i, Event::RECEIVE_START});
    }
}
Pending pending(std::initializer_list<MessageletKey> keys) {
    Pending result;
    for (auto key : keys) result.emplace(key, RoutingDemandPolicy::Ready{key, 0, 24, 100, 0});
    return result;
}
void start(GraExecutionProgress& progress, size_t node, bool comm, uint64_t time = 0) {
    progress.receive({0, static_cast<uint32_t>(node), 0, GraTaskEvent::START, 1, time});
    if (comm) progress.receive({0, static_cast<uint32_t>(100 + node), 0, GraTaskEvent::START, 1, time});
}

void checkNearestJoinAndStartOnlyWork() {
    // Static D favors A because A START releases independent long work. Only
    // A/B FINISH gate Join 0; its slower branch B should instead be protected.
    G::Dag dag{0, {{Type::COMM, 10, 10, {2}}, {Type::COMM, 20, 20, {2}},
        {Type::COMP, 0, 1000, {4}}, {Type::COMP, 0, 5000, {}},
        {Type::COMP, 0, 0, {}}}, {{0, 3, Event::START}}};
    bind(dag);
    const auto graph = G::build({dag}, bps);
    GraExecutionProgress progress(graph);
    const auto policy = makeRoutingPriorityStrategy("join_slack", *graph);
    const auto demands = pending({{0, 10, 0}, {0, 20, 0}});
    auto estimates = policy->evaluate(progress, 0, demands);
    const auto& a = estimates.at({0, 10, 0});
    const auto& b = estimates.at({0, 20, 0});
    assert(graph->dTable().at({0, 10}) > graph->dTable().at({0, 20}));
    assert(a.join_id == 0 && b.join_id == 0);
    assert(a.remaining_ns == 10 && a.peer_remaining_ns == 20 && a.slack_ns == 10);
    assert(b.remaining_ns == 20 && b.slack_ns == -10 && b.priority > a.priority);
    assert(a.frontier_events > 0 && !a.finish_at_frontier);
    const auto legacy = makeRoutingPriorityStrategy("dag_downstream", *graph)->evaluate(progress, 0, demands);
    assert(legacy.at({0, 10, 0}).priority > legacy.at({0, 20, 0}).priority);
    start(progress, 0, true);
    start(progress, 1, true);
    estimates = policy->evaluate(progress, 5, demands);
    assert(estimates.at({0, 10, 0}).finish_at_frontier);
    assert(estimates.at({0, 10, 0}).remaining_ns == 5);
    assert(estimates.at({0, 20, 0}).remaining_ns == 15);
    assert(progress.confirmedEvents().size() == 4); // prediction confirms nothing
    progress.receive({0, 1, 0, GraTaskEvent::FINISH, 2, 20});
    estimates = policy->evaluate(progress, 20, demands);
    assert(estimates.at({0, 20, 0}).priority == 0 && estimates.at({0, 20, 0}).scope == "completed");
    assert(estimates.at({0, 10, 0}).remaining_ns == 1); // overdue, still not FINISHED
    assert(estimates.at({0, 10, 0}).peer_remaining_ns == 0);
}

void checkElapsedProgressChangesClaimOwner() {
    G::Dag dag{0, {{Type::COMM, 10, 10, {2}}, {Type::COMP, 0, 100, {2}},
        {Type::COMP, 0, 0, {}}, {Type::COMM, 20, 100, {5}},
        {Type::COMP, 0, 200, {5}}, {Type::COMP, 0, 0, {}}}};
    bind(dag);
    const auto graph = G::build({dag}, bps);
    GraExecutionProgress progress(graph);
    for (auto node : {0, 1, 3, 4}) start(progress, node, node == 0 || node == 3);
    const auto strategy = makeRoutingPriorityStrategy("join_slack", *graph);
    auto demand = makeRoutingDemandPolicy("dag_priority");
    for (const auto& item : pending({{0, 10, 0}, {0, 20, 0}})) demand->ready(item.second);
    const RoutingDemandPolicy::Corridor corridor{1, 14};
    const auto refresh = [&](uint64_t time) {
        const auto estimates = strategy->evaluate(progress, time, demand->pending());
        demand->refresh([&](const auto&) { return std::optional<RoutingDemandPolicy::Corridor>{corridor}; },
            [&](const auto& ready) { return estimates.at(ready.key).priority; });
        return estimates;
    };
    auto early = refresh(0);
    assert(early.at({0, 10, 0}).slack_ns == 90);
    assert(early.at({0, 20, 0}).slack_ns == 100);
    assert(early.at({0, 10, 0}).relative_slack > early.at({0, 20, 0}).relative_slack);
    assert(demand->claims().at(corridor).owner.key.flow_id == 20);
    auto later = refresh(100);
    assert(later.at({0, 10, 0}).slack_ns == 0);
    assert(later.at({0, 20, 0}).slack_ns == 99);
    assert(demand->claims().at(corridor).owner.key.flow_id == 10);
    assert(demand->pressure({0, 20, 0}, corridor) == 100);
    assert(demand->pressure({0, 10, 0}, corridor) == 0);
    // Priority is derived state, not immutable READY identity.
    assert(!demand->ready({{0, 10, 0}, 0, 24, 100, 999999}));
}

void checkVirtualJoinAndCrossDagNormalization() {
    auto graph = G::build({{0, {{Type::COMM, 10, 10, {}}, {Type::COMM, 20, 20, {}}}},
        {7, {{Type::COMM, 10, 1000, {}}, {Type::COMM, 20, 2000, {}}}},
        {9, {{Type::COMM, 10, 999999, {}}}}}, bps);
    GraExecutionProgress progress(graph);
    const auto strategy = makeRoutingPriorityStrategy("join_slack", *graph);
    auto estimates = strategy->evaluate(progress, 0, pending({{0, 10, 0}, {0, 20, 0},
        {7, 10, 0}, {7, 20, 0}, {9, 10, 0}}));
    assert(estimates.at({0, 10, 0}).scope == "virtual_join");
    assert(!estimates.at({0, 10, 0}).join_id);
    assert(estimates.at({0, 10, 0}).priority == estimates.at({7, 10, 0}).priority);
    assert(estimates.at({0, 20, 0}).priority == estimates.at({7, 20, 0}).priority);
    assert(estimates.at({0, 20, 0}).priority > estimates.at({0, 10, 0}).priority);
    assert(estimates.at({9, 10, 0}).slack_ns == 0); // single branch is limiting, no raw-D advantage
    // A sibling need not be READY, local, or even a communication.
    const auto alone = strategy->evaluate(progress, 0, pending({{0, 10, 0}}));
    assert(alone.at({0, 10, 0}).peer_remaining_ns == 20);
}

void checkRecvGateAndBufferedProgress() {
    G::Dag dag{0, {{Type::COMM, 10, 10, {3}}, {Type::COMM, 20, 20, {3}},
        {Type::COMP, 0, 100, {}}, {Type::COMP, 0, 0, {}}},
        {{2, 0, Event::FINISH, Event::RECEIVE_START}}};
    bind(dag);
    const auto graph = G::build({dag}, bps);
    GraExecutionProgress progress(graph);
    progress.receive({0, 0, 0, GraTaskEvent::START, 1, 0});
    start(progress, 1, true);
    const auto strategy = makeRoutingPriorityStrategy("join_slack", *graph);
    const auto demands = pending({{0, 10, 0}, {0, 20, 0}});
    // A later report is invisible until the missing version arrives.
    assert(progress.receive({0, 2, 0, GraTaskEvent::FINISH, 2, 100}) == GraExecutionProgress::Result::BUFFERED);
    auto before = strategy->evaluate(progress, 20, demands);
    assert(before.at({0, 10, 0}).remaining_ns == 100); // unstarted compute starts at current frontier
    start(progress, 2, false, 0); // drains the buffered FINISH at 100
    auto after = strategy->evaluate(progress, 100, demands);
    assert(after.at({0, 10, 0}).remaining_ns == 1);
    assert(!after.at({0, 10, 0}).finish_at_frontier); // RECV posting remains unconfirmed
    progress.receive({0, 100, 0, GraTaskEvent::START, 1, 200});
    after = strategy->evaluate(progress, 200, demands);
    assert(after.at({0, 10, 0}).remaining_ns == 1); // transfer overlaps delayed RECV, no extra 10 ns
    assert(after.at({0, 10, 0}).finish_at_frontier);
}

void checkTerminalBypassAndEventCycles() {
    const auto graph = G::build({{0, {{Type::COMM, 10, 10, {2, 3}},
        {Type::COMM, 20, 20, {2}}, {Type::COMP, 0, 0, {}}, {Type::COMP, 0, 100, {}}}}}, bps);
    GraExecutionProgress progress(graph);
    const auto estimates = makeRoutingPriorityStrategy("join_slack", *graph)->evaluate(
        progress, 0, pending({{0, 10, 0}, {0, 20, 0}}));
    assert(estimates.at({0, 10, 0}).scope == "virtual_join"); // bypass limits final completion
    assert(estimates.at({0, 10, 0}).remaining_ns == 110);
    assert(estimates.at({0, 20, 0}).scope == "join");
    assert(estimates.at({0, 10, 0}).priority > estimates.at({0, 20, 0}).priority);
    // Logical node projection is cyclic, but START-to-RECEIVE event edges are
    // legal and must not cause node-level traversal loops or false Joins.
    const auto crossed = G::build({{0, {{Type::COMM, 10, 10, {}}, {Type::COMM, 20, 20, {}}},
        {{0, 1, Event::START, Event::RECEIVE_START}, {1, 0, Event::START, Event::RECEIVE_START}}}}, bps);
    GraExecutionProgress crossed_progress(crossed);
    const auto crossed_estimates = makeRoutingPriorityStrategy("join_slack", *crossed)->evaluate(
        crossed_progress, 0, pending({{0, 10, 0}, {0, 20, 0}}));
    assert(crossed_estimates.at({0, 10, 0}).remaining_ns == 10);
    assert(crossed_estimates.at({0, 20, 0}).remaining_ns == 20);
}
}

int main() {
    checkNearestJoinAndStartOnlyWork();
    checkElapsedProgressChangesClaimOwner();
    checkVirtualJoinAndCrossDagNormalization();
    checkRecvGateAndBufferedProgress();
    checkTerminalBypassAndEventCycles();
    std::cout << "routing priority tests passed\n";
}
