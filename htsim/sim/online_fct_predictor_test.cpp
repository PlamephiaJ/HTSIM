#ifdef NDEBUG
#undef NDEBUG
#endif

#include <algorithm>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include "routing_priority.h"
#include "online_fct_predictor.h"

namespace {
using G = GraStaticGraph;
using Type = G::Type;
using Event = G::Event;
using Horizon = OnlineFctPredictor::Horizon;
using Pending = std::map<MessageletKey, RoutingDemandPolicy::Ready>;
constexpr uint64_t bps = 8000000000ULL; // one payload byte/ns

Pending pending(std::initializer_list<MessageletKey> keys) {
    Pending result;
    for (auto key : keys) result.emplace(key, RoutingDemandPolicy::Ready{key, 0, 24, 100, 0});
    return result;
}
void bind(G::Dag& dag) {
    for (size_t i = 0; i < dag.nodes.size(); ++i) {
        dag.tasks.emplace(G::TaskKey{static_cast<uint32_t>(i), 0}, G::TaskBinding{i, Event::START});
        if (dag.nodes[i].type == Type::COMM)
            dag.tasks.emplace(G::TaskKey{static_cast<uint32_t>(100 + i), 0}, G::TaskBinding{i, Event::RECEIVE_START});
    }
}
template<class F> void invalid(F function) {
    bool rejected = false;
    try { function(); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}

void checkWindowIsolationAndVisibility() {
    G::Dag dag{0, {}};
    for (uint64_t id = 0; id < 11; ++id) dag.nodes.push_back({Type::COMM, id, 10, {}, 3, 0});
    dag.nodes.push_back({Type::COMM, 11, 11, {}, 3, 1});
    dag.nodes.push_back({Type::COMM, 12, 12, {}, 3, 1});
    dag.nodes.push_back({Type::COMM, 13, 13, {}, 3});
    dag.nodes.push_back({Type::COMM, 20, 20, {}, 4, 0});
    auto graph = G::build({dag, {7, {{Type::COMM, 0, 30, {}, 3, 0}}}}, bps);
    OnlineFctPredictor predictor(*graph);
    assert(predictor.predict({0, 10}, 0).nominal_fallback);
    assert(predictor.predict({0, 10}, 0).fct_ns == 10);
    // No START or in-flight report trains the model; future completion is rejected.
    invalid([&] { predictor.observeCompletion({0, 0}, 0, 10000, 9999); });
    invalid([&] { predictor.observeCompletion({0, 0}, 2, 1, 2); });
    assert(predictor.predict({0, 10}, 100).sample_count == 0);
    for (uint64_t id = 0; id < 9; ++id) {
        const uint64_t end = 100000 + id * 10000;
        assert(predictor.observeCompletion({0, id}, end - (id + 1) * 1000, end, end));
        assert(!predictor.observeCompletion({0, id}, end - (id + 1) * 1000, end, end));
        const auto prediction = predictor.predict({0, 10}, end / 1000);
        assert(prediction.sample_count == std::min<uint64_t>(id + 1, 8));
        assert(prediction.fct_ns == (id < 8 ? (id + 2) / 2.0L : 5.5L));
        assert(!prediction.nominal_fallback);
    }
    assert(predictor.predict({0, 10}, 99).nominal_fallback); // observation not yet visible
    assert(predictor.predict({0, 12}, 200).nominal_fallback); // other position
    assert(predictor.predict({0, 13}, 200).nominal_fallback); // legacy window is separate from position zero
    assert(predictor.predict({0, 20}, 200).fct_ns == 20); // other collective
    assert(predictor.predict({7, 0}, 200).fct_ns == 30); // same collective ID, different DAG
    assert(predictor.observeCompletion({0, 20}, 100001, 110501, 200000));
    assert(predictor.predict({0, 20}, 200).fct_ns == 10.5L); // ps conversion
    assert(predictor.observeCompletion({0, 11}, 185000, 200000, 200000));
    const auto position_peer = predictor.predict({0, 12}, 200);
    assert(position_peer.comm_position == 1 && position_peer.sample_count == 1 && position_peer.fct_ns == 15);
    assert(predictor.predict({0, 10}, 200).comm_position == 0);
    assert(predictor.predict({0, 10}, 200).fct_ns == 5.5L);
    assert(predictor.observeCompletion({7, 0}, 100000, 150000, 200000));
    assert(predictor.predict({7, 0}, 200).fct_ns == 50);
    assert(predictor.predict({0, 10}, 200).fct_ns == 5.5L);
    const auto lifetime = predictor.predict({0, 10}, 200, Horizon::LongTerm);
    assert(lifetime.fct_ns == 5 && lifetime.sample_count == 9 && !lifetime.nominal_fallback);
    const auto first = predictor.predict({0, 10}, 100, Horizon::LongTerm);
    assert(first.fct_ns == 1 && first.sample_count == 1);
    assert(predictor.predict({0, 10}, 99, Horizon::LongTerm).nominal_fallback);
    assert(predictor.predict({0, 12}, 200, Horizon::LongTerm).sample_count == 1);
    assert(predictor.predict({0, 13}, 200, Horizon::LongTerm).nominal_fallback);
    OnlineFctPredictor independent(*graph);
    assert(independent.predict({0, 10}, 200).nominal_fallback); // no simulation state leak
}

void checkOwnFctVisibilityAndRetention() {
    G::Dag dag{0, {}};
    for (uint64_t id = 0; id < 11; ++id) dag.nodes.push_back({Type::COMM, id, 10, {}, 0});
    auto graph = G::build({dag}, bps);
    OnlineFctPredictor predictor(*graph);
    assert(predictor.observeCompletion({0, 0}, 0, 16000, 16000));
    assert(predictor.observeCompletion({0, 1}, 12000, 20000, 20000));
    // Before its own completion is visible, only the earlier peer is known.
    assert(predictor.predict({0, 1}, 19).fct_ns == 16);
    assert(predictor.predict({0, 1}, 19).sample_count == 1);
    const auto actual = predictor.predict({0, 1}, 20);
    assert(actual.fct_ns == 8 && actual.sample_count == 2 && !actual.nominal_fallback);
    assert(predictor.predict({0, 10}, 20).fct_ns == 12); // unfinished peer still uses mean
    assert(!predictor.observeCompletion({0, 1}, 0, 30000, 30000));
    assert(predictor.predict({0, 1}, 30).fct_ns == 8); // duplicate cannot overwrite actual
    assert(predictor.predict({0, 10}, 30).fct_ns == 12); // or add another sample
    for (uint64_t id = 2; id < 10; ++id) {
        const uint64_t end_ps = (100 + id) * 1000;
        assert(predictor.observeCompletion({0, id}, 80000, end_ps, end_ps));
    }
    // Both early observations have left the eight-sample collective window.
    const auto retained = predictor.predict({0, 1}, 110);
    assert(retained.fct_ns == 8 && retained.sample_count == 8 && !retained.nominal_fallback);
    assert(predictor.predict({0, 0}, 110).fct_ns == 16);
    assert(predictor.predict({0, 10}, 110).fct_ns == 25.5L);
    assert(!predictor.observeCompletion({0, 1}, 0, 110000, 110000));
    assert(predictor.predict({0, 10}, 110).fct_ns == 25.5L);
}

void checkKnownNetworkFctUpdatesGatedFinish() {
    G::Dag dag{0, {{Type::COMM, 10, 10, {3}, 0}, {Type::COMP, 0, 30, {}},
        {Type::COMM, 20, 10, {}, 0}, {Type::COMP, 0, 5, {}},
        {Type::COMP, 0, 100, {}}, {Type::COMM, 30, 10, {}, 0}},
        {{1, 0, Event::FINISH, Event::RECEIVE_START}}};
    bind(dag);
    const auto graph = G::build({dag}, bps);
    GraExecutionProgress progress(graph);
    progress.receive({0, 0, 0, GraTaskEvent::START, 1, 20});
    progress.receive({0, 1, 0, GraTaskEvent::START, 1, 0});
    progress.receive({0, 2, 0, GraTaskEvent::START, 1, 0});
    progress.receive({0, 102, 0, GraTaskEvent::START, 1, 0});
    progress.receive({0, 2, 0, GraTaskEvent::FINISH, 2, 16});
    progress.receive({0, 4, 0, GraTaskEvent::START, 1, 0});
    auto predictor = std::make_shared<OnlineFctPredictor>(*graph);
    auto strategy = makeRoutingPriorityStrategy("global_slack_online", *graph, predictor);
    assert(predictor->observeCompletion({0, 20}, 0, 16000, 16000));
    const auto demands = pending({{0, 10, 0}, {0, 30, 0}});
    const auto before = strategy->evaluate(progress, 28, demands);
    assert(before.at({0, 10, 0}).estimated_finish_time_ns == 36);
    assert(before.at({0, 10, 0}).global_slack_ns == 59);
    assert(predictor->observeCompletion({0, 10}, 20000, 28000, 28000));
    const auto confirmed = progress.confirmedEvents();
    const auto after = strategy->evaluate(progress, 28, demands);
    const auto& own = after.at({0, 10, 0});
    assert(own.predicted_fct_ns == 8 && own.fct_sample_count == 2);
    assert(own.estimated_finish_time_ns == 30); // max(20 + actual 8, RECV readiness 30)
    assert(own.dynamic_tail_ns == 5 && own.estimated_workload_end_ns == 100);
    assert(own.global_slack_ns == 65);
    assert(after.at({0, 30, 0}).predicted_fct_ns == 12); // collective mean remains available
    assert(after.at({0, 30, 0}).estimated_finish_time_ns == 40);
    assert(!progress.eventTime({0, 0, Event::FINISH}));
    assert(progress.confirmedEvents() == confirmed);
    const auto overdue = strategy->evaluate(progress, 40, demands).at({0, 10, 0});
    assert(overdue.predicted_fct_ns == 8 && overdue.estimated_finish_time_ns == 41);
}

void checkColdStartAndDynamicTail() {
    // A -> intermediate Join -> C -> D -> final Join; B -> final Join. C/D are never READY
    // in this test. A initially has 40 ns slack; training C/D makes A limiting.
    G::Dag dag{0, {{Type::COMM, 10, 10, {6}, 0}, {Type::COMM, 20, 70, {4}, 1},
        {Type::COMM, 30, 10, {3}, 2}, {Type::COMM, 40, 10, {4}, 2},
        {Type::COMP, 0, 5, {}}, {Type::COMM, 50, 1, {6}, 2}, {Type::COMP, 0, 0, {2}}}};
    bind(dag);
    auto graph = G::build({dag}, bps);
    GraExecutionProgress progress(graph);
    progress.receive({0, 0, 0, GraTaskEvent::START, 1, 100});
    progress.receive({0, 100, 0, GraTaskEvent::START, 1, 100});
    progress.receive({0, 5, 0, GraTaskEvent::START, 1, 60});
    progress.receive({0, 105, 0, GraTaskEvent::START, 1, 60});
    progress.receive({0, 5, 0, GraTaskEvent::FINISH, 2, 100});
    auto predictor = std::make_shared<OnlineFctPredictor>(*graph);
    auto online = makeRoutingPriorityStrategy("global_slack_online", *graph, predictor);
    auto baseline = makeRoutingPriorityStrategy("global_slack", *graph);
    const auto demands = pending({{0, 10, 0}, {0, 10, 1}, {0, 20, 0}});
    const auto before = online->evaluate(progress, 100, demands);
    const auto nominal = baseline->evaluate(progress, 100, demands);
    for (const auto& item : nominal) {
        const auto& cold = before.at(item.first);
        const auto& old = item.second;
        assert(cold.estimated_finish_time_ns == old.estimated_finish_time_ns);
        assert(cold.dynamic_tail_ns == old.nominal_tail_ns);
        assert(cold.estimated_workload_end_ns == old.estimated_workload_end_ns);
        assert(cold.global_slack_ns == old.global_slack_ns);
        assert(cold.relative_slack == old.relative_slack && cold.priority == old.priority);
        assert(cold.nominal_fallback == true && cold.fct_sample_count == 0);
    }
    assert(before.at({0, 10, 0}).estimated_finish_time_ns == 110);
    assert(before.at({0, 10, 0}).dynamic_tail_ns == 25);
    assert(before.at({0, 10, 0}).estimated_workload_end_ns == 175);
    assert(before.at({0, 10, 0}).global_slack_ns == 40);
    assert(before.at({0, 20, 0}).priority > before.at({0, 10, 0}).priority);
    assert(predictor->observeCompletion({0, 50}, 60000, 100000, 100000));
    const auto confirmed = progress.confirmedEvents();
    const auto after = online->evaluate(progress, 100, demands);
    const auto& a = after.at({0, 10, 0});
    assert(a.estimated_finish_time_ns == 110); // own collective unchanged
    assert(a.dynamic_tail_ns == 85 && a.nominal_tail_ns == 25);
    assert(a.estimated_workload_end_ns == 195 && a.global_slack_ns == 0);
    assert(after.at({0, 20, 0}).global_slack_ns == 20);
    assert(a.priority > after.at({0, 20, 0}).priority);
    assert(a.priority == after.at({0, 10, 1}).priority); // no new tie breaker
    const auto downstream = online->evaluate(progress, 100, pending({{0, 30, 0}, {0, 40, 0}, {0, 50, 0}}));
    assert(downstream.at({0, 30, 0}).estimated_finish_time_ns == 150);
    assert(downstream.at({0, 40, 0}).estimated_finish_time_ns == 190);
    assert(downstream.at({0, 30, 0}).predicted_fct_ns == 40);
    assert(downstream.at({0, 30, 0}).fct_sample_count == 1);
    assert(downstream.at({0, 50, 0}).scope == "completed" && downstream.at({0, 50, 0}).priority == 0);
    assert(progress.confirmedEvents() == confirmed); // predictions do not confirm events
    assert(progress.eventTime({0, 5, Event::FINISH}) == 100);
    // Multiple readers/shared strategies cannot retrain history.
    auto other_gra = makeRoutingPriorityStrategy("global_slack_online", *graph, predictor);
    assert(other_gra->evaluate(progress, 100, demands).at({0, 10, 0}).dynamic_tail_ns == 85);
    assert(predictor->predict({0, 30}, 100).sample_count == 1);
    const auto unchanged = baseline->evaluate(progress, 100, demands);
    for (const auto& item : nominal) {
        const auto& current = unchanged.at(item.first);
        assert(current.estimated_finish_time_ns == item.second.estimated_finish_time_ns);
        assert(current.nominal_tail_ns == item.second.nominal_tail_ns);
        assert(current.global_slack_ns == item.second.global_slack_ns && current.priority == item.second.priority);
    }
    // Elapsed/overdue handling remains active even when a trained FCT is short.
    const auto overdue = online->evaluate(progress, 200, demands);
    assert(overdue.at({0, 10, 0}).estimated_finish_time_ns == 201);
    assert(!progress.eventTime({0, 0, Event::FINISH}));
}

void checkReceiveGateAndStartDependencies() {
    G::Dag dag{0, {{Type::COMM, 10, 10, {3}, 0}, {Type::COMP, 0, 100, {}},
        {Type::COMM, 20, 10, {}, 0}, {Type::COMP, 0, 5, {}}, {Type::COMP, 0, 300, {}}},
        {{1, 0, Event::FINISH, Event::RECEIVE_START}, {0, 4, Event::START}}};
    bind(dag);
    auto graph = G::build({dag}, bps);
    GraExecutionProgress progress(graph);
    progress.receive({0, 0, 0, GraTaskEvent::START, 1, 0});
    progress.receive({0, 1, 0, GraTaskEvent::START, 1, 0});
    progress.receive({0, 4, 0, GraTaskEvent::START, 1, 0});
    auto predictor = std::make_shared<OnlineFctPredictor>(*graph);
    auto strategy = makeRoutingPriorityStrategy("global_slack_online", *graph, predictor);
    const auto demands = pending({{0, 10, 0}});
    assert(predictor->observeCompletion({0, 20}, 0, 20000, 20000));
    const auto estimate = strategy->evaluate(progress, 20, demands).at({0, 10, 0});
    assert(estimate.predicted_fct_ns == 20 && estimate.estimated_finish_time_ns == 100);
    assert(estimate.dynamic_tail_ns == 5); // excludes START-released independent work
    assert(estimate.estimated_workload_end_ns == 300 && estimate.global_slack_ns == 195);
    // The actual network flow can complete while GOAL's RECV gate is blocked.
    assert(predictor->observeCompletion({0, 10}, 0, 30000, 30000));
    assert(!progress.eventTime({0, 0, Event::FINISH}));
    const auto gated = strategy->evaluate(progress, 30, demands).at({0, 10, 0});
    assert(gated.predicted_fct_ns == 30 && gated.estimated_finish_time_ns == 100);
    assert(gated.fct_sample_count == 2); // RECV wait is not a third sample
    progress.receive({0, 100, 0, GraTaskEvent::START, 1, 200});
    const auto waiting = strategy->evaluate(progress, 200, demands).at({0, 10, 0});
    assert(waiting.estimated_finish_time_ns == 201); // no artificial FINISH
    assert(waiting.fct_sample_count == 2 && waiting.predicted_fct_ns == 30);
    progress.receive({0, 0, 0, GraTaskEvent::FINISH, 2, 220});
    assert(strategy->evaluate(progress, 220, demands).at({0, 10, 0}).scope == "completed");
    assert(predictor->predict({0, 10}, 220).fct_ns == 30); // GOAL FINISH does not train
}

void checkStartedAndFuturePredictionHorizons() {
    G::Dag dag{0, {{Type::COMM, 10, 10, {1}, 3, 0}, {Type::COMM, 20, 10, {2}, 3, 0},
                   {Type::COMM, 30, 10, {}, 3, 0}}};
    for (uint64_t id = 100; id < 109; ++id) dag.nodes.push_back({Type::COMM, id, 10, {}, 3, 0});
    bind(dag);
    const auto graph = G::build({dag}, bps);
    GraExecutionProgress progress(graph);
    auto predictor = std::make_shared<OnlineFctPredictor>(*graph);
    auto online = makeRoutingPriorityStrategy("global_slack_online", *graph, predictor);
    for (uint32_t i = 0; i < 9; ++i) {
        const uint64_t end = 100 + i;
        const uint64_t start = end - (i == 0 ? 100 : 10);
        progress.receive({0, i + 3, 0, GraTaskEvent::START, 1, start});
        progress.receive({0, i + 103, 0, GraTaskEvent::START, 1, start});
        progress.receive({0, i + 3, 0, GraTaskEvent::FINISH, 2, end});
        assert(predictor->observeCompletion({0, 100 + i}, start * 1000, end * 1000, end * 1000));
    }

    progress.receive({0, 0, 0, GraTaskEvent::START, 1, 120});
    progress.receive({0, 101, 0, GraTaskEvent::START, 1, 120}); // B's RECV starts before its SEND
    const auto demands = pending({{0, 10, 0}, {0, 20, 0}, {0, 30, 0}});
    const auto before = online->evaluate(progress, 120, demands);
    const auto& a = before.at({0, 10, 0});
    const auto& b = before.at({0, 20, 0});
    assert(a.fct_horizon == "short_term" && a.predicted_fct_ns == 10 && a.fct_sample_count == 8);
    assert(a.estimated_finish_time_ns == 130 && a.dynamic_tail_ns == 40 && a.estimated_workload_end_ns == 170);
    assert(b.fct_horizon == "long_term" && b.predicted_fct_ns == 20 && b.fct_sample_count == 9);
    assert(b.estimated_finish_time_ns == 150 && b.dynamic_tail_ns == 20);

    assert(predictor->observeCompletion({0, 10}, 120000, 130000, 130000));
    progress.receive({0, 0, 0, GraTaskEvent::FINISH, 2, 130});
    progress.receive({0, 1, 0, GraTaskEvent::START, 1, 130});
    const auto after = online->evaluate(progress, 130, demands);
    const auto& started = after.at({0, 20, 0});
    assert(started.fct_horizon == "short_term" && started.predicted_fct_ns == 10 && started.fct_sample_count == 8);
    assert(started.estimated_finish_time_ns == 140 && started.dynamic_tail_ns == 19);
    const auto& future = after.at({0, 30, 0});
    assert(future.fct_horizon == "long_term" && future.predicted_fct_ns == 19 && future.fct_sample_count == 10);
    assert(future.estimated_finish_time_ns == 159 && future.estimated_workload_end_ns == 159);
}

void checkComponentCacheAcrossTimeAndProgressViews() {
    G::Dag dag{0, {{Type::COMM, 10, 10, {}, 0}, {Type::COMM, 20, 20, {}, 0}}};
    bind(dag);
    auto graph = G::build({dag}, bps);
    assert(graph->componentCount() == 2);
    auto versions = std::make_shared<GraExecutionProgress::ConfirmationVersions>(graph->componentCount());
    GraExecutionProgress local(graph, versions), remote(graph, versions);
    local.receive({0, 0, 0, GraTaskEvent::START, 1, 30});
    remote.receive({0, 0, 0, GraTaskEvent::START, 1, 31});
    for (auto* progress : {&local, &remote}) {
        progress->receive({0, 100, 0, GraTaskEvent::START, 1, 30});
        progress->receive({0, 1, 0, GraTaskEvent::START, 1, 0});
    }
    for (auto* progress : {&local, &remote}) progress->receive({0, 101, 0, GraTaskEvent::START, 1, 0});
    const auto local_component = graph->eventComponent(graph->eventIndex({0, 0, Event::START}));
    const auto shared_component = graph->eventComponent(graph->eventIndex({0, 1, Event::START}));
    assert(local.componentVersion(local_component) != remote.componentVersion(local_component));
    assert(local.componentVersion(shared_component) == remote.componentVersion(shared_component));
    auto predictor = std::make_shared<OnlineFctPredictor>(*graph);
    const auto online = makeRoutingPriorityStrategy("global_slack_online", *graph, predictor);
    const auto requests = pending({{0, 10, 0}, {0, 20, 0}});
    const auto before = online->evaluate(local, 39, requests).at({0, 10, 0});
    assert(before.nominal_fallback == true && before.estimated_finish_time_ns == 40);
    assert(before.global_end_node_index == 1); // Equal terminal ETAs keep reverse topological order.
    assert(online->evaluate(local, 40, requests).at({0, 10, 0}).nominal_fallback == true);

    // The FCT window spans the two components. A sample arriving at the
    // same time as an estimate invalidates both, even with unchanged progress.
    assert(predictor->observeCompletion({0, 20}, 0, 35000, 40000));
    const auto after = online->evaluate(local, 40, requests).at({0, 10, 0});
    assert(after.predicted_fct_ns == 35 && after.estimated_finish_time_ns == 65);
    assert(after.global_end_node_index == 0 && after.global_slack_ns == 0);
    const auto other = online->evaluate(remote, 40, requests).at({0, 10, 0});
    assert(other.predicted_fct_ns == 35 && other.estimated_finish_time_ns == 66);
    assert(online->evaluate(local, 40, requests).at({0, 10, 0}).estimated_finish_time_ns == 65);
    assert(online->evaluate(local, 39, requests).at({0, 10, 0}).nominal_fallback == true);
}

void checkMissingMetadata() {
    const auto graph = G::build({{0, {{Type::COMM, 1, 10, {}}}}}, bps);
    invalid([&] { makeRoutingPriorityStrategy("global_slack_online", *graph); });
    assert(makeRoutingPriorityStrategy("global_slack", *graph));
}
} // namespace

int main() {
    checkWindowIsolationAndVisibility();
    checkOwnFctVisibilityAndRetention();
    checkKnownNetworkFctUpdatesGatedFinish();
    checkColdStartAndDynamicTail();
    checkReceiveGateAndStartDependencies();
    checkStartedAndFuturePredictionHorizons();
    checkComponentCacheAcrossTimeAndProgressViews();
    checkMissingMetadata();
    std::cout << "online FCT predictor tests passed\n";
}
