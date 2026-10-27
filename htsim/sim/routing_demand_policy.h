#ifndef ROUTING_DEMAND_POLICY_H
#define ROUTING_DEMAND_POLICY_H

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include "messagelet.h"

// Pre-routing demand only. No packet queues, topology, RNG, DAG traversal or
// commitment accounting belongs here. GRA supplies identities, priorities and
// a base-network preference oracle, so experimental policies can be replaced.
class RoutingDemandPolicy {
public:
    using Corridor = std::pair<uint32_t, uint32_t>;
    struct Ready {
        MessageletKey key;
        uint32_t from, to;
        uint64_t payload_bytes, priority;
    };
    struct Claim { Ready owner; Corridor corridor; };
    struct PressureObservation {
        uint64_t bytes = 0; // eligible original payload, before beta scaling
        std::string reason;
        std::optional<uint64_t> requester_priority;
        std::optional<Claim> claim;
    };
    using Preference = std::function<std::optional<Corridor>(const Ready&)>;
    using Priority = std::function<uint64_t(const Ready&)>;
    virtual ~RoutingDemandPolicy() = default;
    virtual bool ready(const Ready& demand) = 0;
    virtual bool remove(MessageletKey key) = 0; // routed or cancelled
    virtual void refresh(const Preference& preference, const Priority& priority = {}) = 0;
    virtual uint64_t pressure(MessageletKey requester, Corridor corridor) const = 0;
    // Diagnostics belong to the strategy that decides eligibility. Other
    // experimental strategies can supply their own reason codes. Observation
    // bytes must agree with pressure() for the same requester and corridor.
    virtual PressureObservation observePressure(MessageletKey requester, Corridor corridor) const {
        return {pressure(requester, corridor), "policy_defined", std::nullopt, std::nullopt};
    }
    virtual const std::map<MessageletKey, Ready>& pending() const = 0;
    virtual const std::map<Corridor, Claim>& claims() const = 0;
};

std::unique_ptr<RoutingDemandPolicy> makeRoutingDemandPolicy(const std::string& strategy);

#endif
