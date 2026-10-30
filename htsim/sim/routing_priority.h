#ifndef ROUTING_PRIORITY_H
#define ROUTING_PRIORITY_H

#include "gra_execution_progress.h"
#include "routing_demand_policy.h"

class OnlineFctPredictor;

// Priority estimation has no packet, topology, path selection or Commitment
// ownership. It reads the selected GRA's confirmed progress and, for the online
// strategy, shared completed network FCT history. Evaluation never trains it.
class RoutingPriorityStrategy {
public:
    struct Estimate {
        uint64_t priority = 0; // larger wins; dynamic strategy uses ordinal ranks
        std::string strategy;
        std::string scope;
        std::optional<uint64_t> join_id, branch_id;
        std::optional<long double> remaining_ns, peer_remaining_ns, slack_ns, relative_slack;
        std::optional<long double> estimated_finish_time_ns, nominal_tail_ns, estimated_workload_end_ns, global_slack_ns;
        std::optional<uint64_t> global_end_node_index;
        std::optional<uint64_t> collective_id, prediction_time_ns;
        std::optional<long double> predicted_fct_ns, dynamic_tail_ns;
        std::optional<size_t> fct_sample_count;
        std::optional<bool> nominal_fallback;
        size_t frontier_events = 0;
        bool finish_at_frontier = false;
    };
    using Estimates = std::map<MessageletKey, Estimate>;
    virtual ~RoutingPriorityStrategy() = default;
    virtual bool usesProgress() const { return false; }
    virtual Estimates evaluate(const GraExecutionProgress& progress, uint64_t now_ns,
        const std::map<MessageletKey, RoutingDemandPolicy::Ready>& pending) const = 0;
};

std::unique_ptr<RoutingPriorityStrategy> makeRoutingPriorityStrategy(
    const std::string& strategy, const GraStaticGraph& graph,
    std::shared_ptr<const OnlineFctPredictor> predictor = nullptr);

#endif
