#include "routing_demand_policy.h"

#include <stdexcept>
#include <tuple>

namespace {
class DagSoftClaimPolicy final : public RoutingDemandPolicy {
public:
    bool ready(const Ready& demand) override {
        if (!demand.payload_bytes) throw std::invalid_argument("READY payload must be positive");
        auto inserted = _pending.emplace(demand.key, demand);
        if (!inserted.second) {
            const auto& old = inserted.first->second;
            if (std::tie(old.from, old.to, old.payload_bytes, old.priority) !=
                std::tie(demand.from, demand.to, demand.payload_bytes, demand.priority))
                throw std::logic_error("Inconsistent duplicate READY");
        }
        return inserted.second;
    }
    bool remove(MessageletKey key) override { return _pending.erase(key) != 0; }
    void refresh(const Preference& preference) override {
        _claims.clear();
        // Ordered keys give deterministic ownership for equal priorities;
        // equals never exert pressure on each other.
        for (const auto& item : _pending) {
            const auto& demand = item.second;
            const auto corridor = preference(demand);
            if (!corridor) continue;
            auto found = _claims.find(*corridor);
            if (found == _claims.end()) _claims.emplace(*corridor, Claim{demand, *corridor});
            else if (demand.priority > found->second.owner.priority)
                found->second = {demand, *corridor};
        }
    }
    uint64_t pressure(MessageletKey requester, Corridor corridor) const override {
        return observePressure(requester, corridor).bytes;
    }
    PressureObservation observePressure(MessageletKey requester, Corridor corridor) const override {
        PressureObservation result;
        const auto current = _pending.find(requester);
        const auto claim = _claims.find(corridor);
        if (current != _pending.end()) result.requester_priority = current->second.priority;
        if (claim != _claims.end()) result.claim = claim->second;
        if (current == _pending.end()) result.reason = "missing_ready";
        else if (claim == _claims.end()) result.reason = "no_claim";
        else if (claim->second.owner.key == requester) result.reason = "self_claim";
        else if (claim->second.owner.priority == current->second.priority) result.reason = "equal_priority";
        else if (claim->second.owner.priority < current->second.priority) result.reason = "lower_priority";
        else {
            result.reason = "applied";
            result.bytes = claim->second.owner.payload_bytes;
        }
        return result;
    }
    const std::map<MessageletKey, Ready>& pending() const override { return _pending; }
    const std::map<Corridor, Claim>& claims() const override { return _claims; }
private:
    std::map<MessageletKey, Ready> _pending;
    std::map<Corridor, Claim> _claims;
};
}

std::unique_ptr<RoutingDemandPolicy> makeRoutingDemandPolicy(const std::string& strategy) {
    if (strategy == "dag_priority") return std::make_unique<DagSoftClaimPolicy>();
    throw std::invalid_argument("Unsupported soft claim policy '" + strategy + "'");
}
