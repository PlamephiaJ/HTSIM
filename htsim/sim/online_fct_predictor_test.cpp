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
    for (uint64_t id = 0; id < 11; ++id) dag.nodes.push_back({Type::COMM, id, 10, {}, 3});
    dag.nodes.push_back({Type::COMM, 20, 20, {}, 4});
    auto graph = G::build({dag, {7, {{Type::COMM, 0, 30, {}, 3}}}}, bps);
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
    assert(predictor.predict({0, 20}, 200).fct_ns == 20); // other collective
    assert(predictor.predict({7, 0}, 200).fct_ns == 30); // same collective ID, different DAG
    assert(predictor.observeCompletion({0, 20}, 100001, 110501, 200000));
    assert(predictor.predict({0, 20}, 200).fct_ns == 10.5L); // ps conversion
    assert(predictor.predict({0, 10}, 200).fct_ns == 5.5L);
    assert(predictor.observeCompletion({7, 0}, 100000, 150000, 200000));
    assert(predictor.predict({7, 0}, 200).fct_ns == 50);
    assert(predictor.predict({0, 10}, 200).fct_ns == 5.5L);
    OnlineFctPredictor independent(*graph);
    assert(independent.predict({0, 10}, 200).nominal_fallback); // no simulation state leak
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
    assert(gated.predicted_fct_ns == 25 && gated.estimated_finish_time_ns == 100);
    assert(gated.fct_sample_count == 2); // RECV wait is not a third sample
    progress.receive({0, 100, 0, GraTaskEvent::START, 1, 200});
    const auto waiting = strategy->evaluate(progress, 200, demands).at({0, 10, 0});
    assert(waiting.estimated_finish_time_ns == 201); // no artificial FINISH
    assert(waiting.fct_sample_count == 2 && waiting.predicted_fct_ns == 25);
    progress.receive({0, 0, 0, GraTaskEvent::FINISH, 2, 220});
    assert(strategy->evaluate(progress, 220, demands).at({0, 10, 0}).scope == "completed");
    assert(predictor->predict({0, 10}, 220).fct_ns == 25); // GOAL FINISH does not train
}

void checkMissingMetadata() {
    const auto graph = G::build({{0, {{Type::COMM, 1, 10, {}}}}}, bps);
    invalid([&] { makeRoutingPriorityStrategy("global_slack_online", *graph); });
    assert(makeRoutingPriorityStrategy("global_slack", *graph));
}
} // namespace

int main() {
    checkWindowIsolationAndVisibility();
    checkColdStartAndDynamicTail();
    checkReceiveGateAndStartDependencies();
    checkMissingMetadata();
    std::cout << "online FCT predictor tests passed\n";
}
