#ifndef GRA_EXECUTION_PROGRESS_H
#define GRA_EXECUTION_PROGRESS_H

#include <optional>
#include "gra_static_graph.h"

enum class GraTaskEvent { START, FINISH };

// Host reports use original task identities, never internal graph node IDs.
struct GraProgressReport {
    uint64_t dag_index;
    uint32_t rank, task_offset;
    GraTaskEvent event;
    uint64_t version, event_time_ns;
};

// One GRA's visible reports. Readiness never confirms an execution event.
class GraExecutionProgress {
public:
    using EventKey = GraStaticGraph::EventKey;
    enum class Result { APPLIED, DUPLICATE, BUFFERED };
    explicit GraExecutionProgress(std::shared_ptr<const GraStaticGraph> graph);
    Result receive(const GraProgressReport& report);
    const std::map<EventKey, uint64_t>& confirmedEvents() const { return _confirmed; }
    std::optional<uint64_t> eventTime(EventKey key) const;
    uint64_t lastVersion(uint32_t rank) const;
    size_t pendingReports(uint32_t rank) const;
    std::vector<EventKey> frontier(uint64_t dag_index) const;

private:
    std::optional<EventKey> resolve(const GraProgressReport& report) const;
    void apply(const GraProgressReport& report);
    struct RankState {
        uint64_t last_version = 0;
        std::map<uint64_t, GraProgressReport> pending;
    };
    std::shared_ptr<const GraStaticGraph> _graph;
    std::map<uint32_t, RankState> _ranks;
    std::map<EventKey, uint64_t> _confirmed;
};

#endif
