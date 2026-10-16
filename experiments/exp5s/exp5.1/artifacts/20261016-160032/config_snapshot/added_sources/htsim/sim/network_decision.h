#ifndef NETWORK_DECISION_H
#define NETWORK_DECISION_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>

class DragonflyTopology;

struct NetworkRoute {
    uint32_t path_id = 0;
    std::string description = "minimum";
    std::map<std::string, std::string> log_info;
};

struct NetworkDecisionContext {
    DragonflyTopology& topology;
    uint32_t from;
    uint32_t to;
    uint32_t tag;
    uint64_t flow_size;
    uint64_t messagelet_size;
    uint64_t messagelet_index;
};

// Called before a messagelet is labelled. The runtime owns and freezes the
// returned SOURCE route; a policy never changes packets already in flight.
class NetworkDecision {
public:
    virtual ~NetworkDecision() = default;
    virtual NetworkRoute decide(const NetworkDecisionContext& context) = 0;
};

class UgalLNetworkDecision final : public NetworkDecision {
public:
    NetworkRoute decide(const NetworkDecisionContext& context) override;
};

// "none" preserves the runtime's configured/static routes.
std::unique_ptr<NetworkDecision> makeNetworkDecision(const std::string& strategy);

#endif // NETWORK_DECISION_H
