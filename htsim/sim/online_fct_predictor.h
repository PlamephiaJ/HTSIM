#ifndef ONLINE_FCT_PREDICTOR_H
#define ONLINE_FCT_PREDICTOR_H

#include <deque>
#include <set>
#include "gra_static_graph.h"

// One simulation's network completion history, shared by all its GRAs.
// Only the runtime writes observations; evaluating priorities is read-only.
class OnlineFctPredictor {
public:
    using Flow = GraStaticGraph::FlowKey;
    struct Prediction {
        uint64_t collective_id;
        long double fct_ns;
        size_t sample_count;
        bool nominal_fallback;
    };
    explicit OnlineFctPredictor(const GraStaticGraph& graph);
    // Times are HTSim picoseconds. The caller supplies its current event time;
    // incomplete/future flows are rejected. Duplicate completions return false.
    bool observeCompletion(Flow flow, uint64_t start_ps, uint64_t end_ps, uint64_t now_ps);
    Prediction predict(Flow flow, uint64_t now_ns) const;

private:
    struct FlowInfo { uint64_t collective_id, nominal_ns; };
    struct Sample { uint64_t visible_ns; long double fct_ns; };
    static constexpr size_t window_size = 8;
    std::map<Flow, FlowInfo> _flows;
    std::map<std::pair<uint64_t, uint64_t>, std::deque<Sample>> _history;
    std::set<Flow> _completed;
};

#endif
