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
    using Preference = std::function<std::optional<Corridor>(const Ready&)>;
    virtual ~RoutingDemandPolicy() = default;
    virtual bool ready(const Ready& demand) = 0;
    virtual bool remove(MessageletKey key) = 0; // routed or cancelled
    virtual void refresh(const Preference& preference) = 0;
    virtual uint64_t pressure(MessageletKey requester, Corridor corridor) const = 0;
    virtual const std::map<MessageletKey, Ready>& pending() const = 0;
    virtual const std::map<Corridor, Claim>& claims() const = 0;
};

std::unique_ptr<RoutingDemandPolicy> makeRoutingDemandPolicy(const std::string& strategy);

#endif
