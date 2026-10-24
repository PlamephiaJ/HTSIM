#include "gra_execution_progress.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

GraExecutionProgress::GraExecutionProgress(std::shared_ptr<const GraStaticGraph> graph)
    : _graph(std::move(graph)) {
    if (!_graph) throw std::invalid_argument("GRA progress requires a static graph");
}

std::optional<GraExecutionProgress::EventKey> GraExecutionProgress::resolve(
    const GraProgressReport& report) const {
    const auto& dag = _graph->dags().at(report.dag_index);
    const auto& task = dag.tasks.at({report.rank, report.task_offset});
    switch (report.event) {
        case GraTaskEvent::START:
            return EventKey{report.dag_index, task.node_index, task.start_event};
        case GraTaskEvent::FINISH:
            // GOAL marks both endpoints done on actual message matching.
            // SEND is the sole authority for the shared logical FINISH. RECV
            // FINISH still consumes its rank's report version, but adds no event.
            if (task.start_event == GraStaticGraph::Event::RECEIVE_START) return std::nullopt;
            return EventKey{report.dag_index, task.node_index, GraStaticGraph::Event::FINISH};
    }
    throw std::invalid_argument("Unknown GRA task progress event");
}

void GraExecutionProgress::apply(const GraProgressReport& report) {
    const auto key = resolve(report);
    if (!key) return;
    const auto found = _confirmed.find(*key);
    if (found != _confirmed.end() && found->second != report.event_time_ns)
        throw std::invalid_argument("Conflicting actual time for a GRA execution event");
    _confirmed.emplace(*key, report.event_time_ns);
}

GraExecutionProgress::Result GraExecutionProgress::receive(const GraProgressReport& report) {
    if (!report.version) throw std::invalid_argument("GRA report versions start at one");
    auto& rank = _ranks[report.rank];
    if (report.version <= rank.last_version || rank.pending.count(report.version)) return Result::DUPLICATE;
    const auto key = resolve(report); // reject invalid identities before accepting or buffering
    if (key && _confirmed.count(*key) && _confirmed.at(*key) != report.event_time_ns)
        throw std::invalid_argument("Conflicting actual time for a GRA execution event");
    rank.pending.emplace(report.version, report);
    bool applied = false;
    while (rank.last_version != std::numeric_limits<uint64_t>::max()) {
        auto next = rank.pending.find(rank.last_version + 1);
        if (next == rank.pending.end()) break;
        try {
            apply(next->second);
        } catch (...) {
            rank.pending.erase(next); // a corrected report may fill this version
            throw;
        }
        rank.last_version = next->first;
        rank.pending.erase(next);
        applied = true;
    }
    return applied ? Result::APPLIED : Result::BUFFERED;
}

std::optional<uint64_t> GraExecutionProgress::eventTime(EventKey key) const {
    const auto found = _confirmed.find(key);
    return found == _confirmed.end() ? std::nullopt : std::optional<uint64_t>{found->second};
}

uint64_t GraExecutionProgress::lastVersion(uint32_t rank) const {
    const auto found = _ranks.find(rank);
    return found == _ranks.end() ? 0 : found->second.last_version;
}

size_t GraExecutionProgress::pendingReports(uint32_t rank) const {
    const auto found = _ranks.find(rank);
    return found == _ranks.end() ? 0 : found->second.pending.size();
}

std::vector<GraExecutionProgress::EventKey> GraExecutionProgress::frontier(uint64_t dag_index) const {
    _graph->dags().at(dag_index); // reject unknown DAGs, including empty graphs
    std::vector<EventKey> result;
    for (const auto& item : _graph->eventPredecessors()) {
        if (item.first.dag_index != dag_index || _confirmed.count(item.first)) continue;
        if (std::all_of(item.second.begin(), item.second.end(),
            [&](const auto& input) { return _confirmed.count(input) != 0; }))
            result.push_back(item.first);
    }
    return result;
}
