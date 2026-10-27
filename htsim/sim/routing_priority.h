#ifndef ROUTING_PRIORITY_H
#define ROUTING_PRIORITY_H

#include "gra_execution_progress.h"
#include "routing_demand_policy.h"

// Priority estimation has no packet, topology, path selection or Commitment
// ownership. All observations are the selected GRA's confirmed progress.
class RoutingPriorityStrategy {
public:
    struct Estimate {
        uint64_t priority = 0; // larger wins; dynamic strategy uses ordinal ranks
        std::string strategy;
        std::string scope;
        std::optional<uint64_t> join_id, branch_id;
        std::optional<long double> remaining_ns, peer_remaining_ns, slack_ns, relative_slack;
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
    const std::string& strategy, const GraStaticGraph& graph);

#endif
