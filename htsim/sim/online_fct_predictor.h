#ifndef ONLINE_FCT_PREDICTOR_H
#define ONLINE_FCT_PREDICTOR_H

#include <deque>
#include "gra_static_graph.h"

// One simulation's network completion history, shared by all its GRAs.
// Only the runtime writes observations; evaluating priorities is read-only.
class OnlineFctPredictor {
public:
    using Flow = GraStaticGraph::FlowKey;
    enum class Horizon { ShortTerm, LongTerm };
    struct Prediction {
        uint64_t collective_id;
        std::optional<uint64_t> comm_position;
        long double fct_ns;
        size_t sample_count; // visible samples for this horizon, even for an actual FCT
        bool nominal_fallback;
        Horizon horizon;
    };
    explicit OnlineFctPredictor(const GraStaticGraph& graph);
    // Times are HTSim picoseconds. The caller supplies its current event time;
    // incomplete/future flows are rejected. Duplicate completions return false.
    bool observeCompletion(Flow flow, uint64_t start_ps, uint64_t end_ps, uint64_t now_ps);
    // Prefer this flow's visible network FCT, then the selected mean, then
    // nominal duration. Legacy flows share history without a position.
    Prediction predict(Flow flow, uint64_t now_ns, Horizon horizon = Horizon::ShortTerm) const;

private:
    using WindowKey = std::tuple<uint64_t, uint64_t, std::optional<uint64_t>>; // DAG, collective, position
    struct FlowInfo { WindowKey window; uint64_t nominal_ns; };
    struct Sample { uint64_t visible_ns; long double fct_ns; };
    struct Totals {
        long double sum_ns = 0;
        size_t count = 0;
        void add(long double fct_ns) { sum_ns += fct_ns; ++count; }
    };
    struct History {
        std::deque<Sample> recent;
        Totals lifetime;
        uint64_t latest_visible_ns = 0;
    };
    static constexpr size_t window_size = 8;
    std::map<Flow, FlowInfo> _flows;
    std::map<WindowKey, History> _history;
    // Retain actual FCT independently of window eviction; also deduplicates
    // network completions while GOAL FINISH may still be unconfirmed.
    std::map<Flow, Sample> _completed;
};

#endif
