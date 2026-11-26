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
    // A shared broadcast can reuse the version of an identical transition.
    // Different predecessors, event IDs or timestamps always get distinct versions.
    class ConfirmationVersions {
    public:
        explicit ConfirmationVersions(size_t components) : _last(components) {}
    private:
        friend class GraExecutionProgress;
        struct Transition { uint64_t prior, time, version; size_t event; };
        uint64_t advance(size_t component, uint64_t prior, size_t event, uint64_t time);
        std::vector<std::optional<Transition>> _last;
    };
    explicit GraExecutionProgress(std::shared_ptr<const GraStaticGraph> graph,
        std::shared_ptr<ConfirmationVersions> versions = nullptr);
    Result receive(const GraProgressReport& report);
    const std::map<EventKey, uint64_t>& confirmedEvents() const { return _confirmed; }
    std::optional<uint64_t> eventTime(EventKey key) const;
    std::optional<uint64_t> indexedTime(size_t index) const { return _times[index]; }
    uint64_t componentVersion(size_t index) const { return _component_versions[index]; }
    const GraStaticGraph& graph() const { return *_graph; }
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
    std::vector<std::optional<uint64_t>> _times;
    std::vector<uint64_t> _component_versions;
    std::shared_ptr<ConfirmationVersions> _versions;
};

#endif
