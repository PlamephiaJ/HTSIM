#ifndef NETWORK_DECISION_H
#define NETWORK_DECISION_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <optional>
#include "messagelet.h"

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
    std::optional<MessageletKey> messagelet = std::nullopt;
};

// Called before a messagelet is labelled. The host runtime or entry switch
// freezes the returned route; packets already in flight keep their decision.
class NetworkDecision {
public:
    virtual ~NetworkDecision() = default;
    virtual NetworkRoute decide(const NetworkDecisionContext& context) = 0;
};

class UgalLNetworkDecision final : public NetworkDecision {
public:
    explicit UgalLNetworkDecision(bool commitment_aware = false, double alpha = 1.0,
                                 std::optional<double> beta = std::nullopt)
        : _commitment_aware(commitment_aware), _alpha(alpha), _beta(beta.value_or(alpha)) {}
    NetworkRoute decide(const NetworkDecisionContext& context) override;
private:
    bool _commitment_aware;
    double _alpha;
    double _beta;
};

// "none" preserves the runtime's configured/static routes.
std::unique_ptr<NetworkDecision> makeNetworkDecision(const std::string& strategy, double alpha = 1.0,
                                                   std::optional<double> beta = std::nullopt);

#endif // NETWORK_DECISION_H
