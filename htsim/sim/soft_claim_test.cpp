#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <iostream>
#include <stdexcept>
#include "routing_demand_policy.h"
#include "datacenter/dragonfly_topology.h"
#include "uec.h"

namespace {
using Corridor = RoutingDemandPolicy::Corridor;
const MessageletKey a{7, 10, 0}, b{7, 20, 0}, equal{7, 30, 0};
const Corridor minimal{1, 14}, other{0, 11};

void checkPolicy() {
    auto policy = makeRoutingDemandPolicy("dag_priority");
    const RoutingDemandPolicy::Ready high{a, 0, 24, 8192, 100};
    const RoutingDemandPolicy::Ready low{b, 0, 24, 4096, 10};
    const RoutingDemandPolicy::Ready tie{equal, 0, 24, 2048, 100};
    assert(policy->ready(low) && policy->ready(high) && policy->ready(tie));
    assert(!policy->ready(high));
    auto preference = [&](const RoutingDemandPolicy::Ready&) { return std::optional<Corridor>{minimal}; };
    policy->refresh(preference);
    assert(policy->pending().size() == 3 && policy->claims().size() == 1);
    assert(policy->claims().at(minimal).owner.key == a);
    assert(policy->pressure(b, minimal) == 8192);
    assert(policy->pressure(a, minimal) == 0 && policy->pressure(equal, minimal) == 0);
    assert(policy->pressure({8, 20, 0}, minimal) == 0); // missing READY / other DAG
    assert(policy->pressure(b, other) == 0);
    assert(policy->remove(a));
    policy->refresh(preference);
    assert(policy->pressure(b, minimal) == 2048); // next highest, no summation
    // A messagelet moves its single claim when the base-network preference changes.
    policy->refresh([&](const RoutingDemandPolicy::Ready& ready) {
        return std::optional<Corridor>{ready.key == equal ? other : minimal};
    });
    assert(policy->claims().size() == 2);
    assert(policy->pressure(b, minimal) == 0 && policy->pressure(b, other) == 2048);
    bool rejected = false;
    try { policy->ready({b, 0, 24, 9999, 10}); } catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
}

void graph(DragonflyTopology& topology) {
    using G = GraStaticGraph;
    topology.initializeGraStaticGraphs({{7, {
        {G::Type::COMM, 10, 8192, {3}}, {G::Type::COMM, 20, 8192, {}},
        {G::Type::COMM, 30, 8192, {}}, {G::Type::COMP, 0, 100000, {}}}}});
}

void checkEntryLifecycle() {
    auto& events = EventList::getTheEventList();
    events.setEndtime(1);
    DragonflySwitch::set_config(DragonflySwitch::ENTRY_SWITCH, false, 64);
    MessageletConfig config;
    config.runtime.routing_mode = MessageletConfig::RoutingMode::ENTRY_SWITCH;
    config.routing.network_decision = "commitment_aware_ugal";
    config.routing.soft_claim.enabled = true;
    config.validate();
    DragonflySwitch::configureEntryRouting(config);
    DragonflyTopology topology(COMPOSITE, 1000000, nullptr, &events);
    topology.construct_topology(2, 4, 2);
    graph(topology);
    auto& gra = topology.groupRoutingAgent(0);
    auto& entry = *static_cast<DragonflySwitch*>(topology.switches[0]);
    gra.setDemandPolicy(makeRoutingDemandPolicy("dag_priority"));
    gra.messageletReady(a, 0, 24, 8192);
    gra.messageletReady(b, 0, 24, 8192);
    assert(gra.query(minimal).r_bytes == 0 && gra.snapshot().messagelets.empty());
    assert(gra.softClaimPressure(b, minimal) == 8192);
    // READY and repeated preference refreshes must preserve the real sample sequence.
    DragonflySwitch reference(events, "reference", DragonflySwitch::GENERAL, 0, 0, &topology);
    for (int i = 0; i < 20; ++i) gra.refreshDemand();
    const auto expected = reference.ugalDecision(0, 12);
    const auto preview = entry.previewUgalDecision(0, 12, {});
    assert(preview.next_val == expected.next_val && preview.h_val == expected.h_val);

    PacketFlow flow_a(nullptr), flow_b(nullptr);
    Route route;
    auto packet = [&](PacketFlow& flow, MessageletKey key, uint64_t seq) {
        auto* p = UecDataPacket::newpkt(flow, route, seq, 4160, UecBasePacket::DATA_PULL, 0, 24);
        p->set_src(0);
        p->set_messagelet({key, {8192, 1}});
        return p;
    };
    auto* low = packet(flow_b, b, 0);
    entry.getNextHop(*low, nullptr); // B arrives first; idle minimal is protected
    const auto b_corridor = *gra.corridorForRoute(0, 24, low->pathid());
    assert(b_corridor != minimal && gra.remaining(b) == 8192);
    assert(gra.demandPolicy()->pending().size() == 1);
    assert(gra.demandPolicy()->claims().at(minimal).owner.key == a);
    auto* high = packet(flow_a, a, 0);
    entry.getNextHop(*high, nullptr);
    assert(gra.corridorForRoute(0, 24, high->pathid()) == minimal);
    assert(gra.remaining(a) == 8192 && gra.demandPolicy()->pending().empty());
    assert(gra.demandPolicy()->claims().empty());
    gra.messageletReady(a, 0, 24, 8192); // delayed report cannot resurrect a claim
    assert(gra.demandPolicy()->pending().empty());
    const auto version = gra.snapshot().version;
    auto* retry = packet(flow_b, b, 0);
    entry.getNextHop(*retry, nullptr);
    assert(retry->pathid() == low->pathid() && gra.snapshot().version == version);
    gra.admitted(b_corridor, *low);
    gra.admitted(b_corridor, *retry);
    assert(gra.remaining(b) == 4096); // retransmission only consumes once
    gra.messageletReady(equal, 0, 24, 8192);
    assert(!gra.demandPolicy()->pending().empty());
    gra.cancelReady(equal);
    assert(gra.demandPolicy()->pending().empty() && gra.demandPolicy()->claims().empty());
    low->free(); high->free(); retry->free();
    events.setEndtime(0);
}

void checkFallbackAndActualPressure() {
    auto& events = EventList::getTheEventList();
    DragonflyTopology topology(COMPOSITE, 1000000, nullptr, &events);
    topology.construct_topology(2, 4, 2);
    graph(topology);
    auto& gra = topology.groupRoutingAgent(0);
    gra.setDemandPolicy(makeRoutingDemandPolicy("dag_priority"));
    gra.messageletReady(a, 0, 24, 8192);
    UgalLNetworkDecision policy(true);
    const auto missing = policy.decide({topology, 0, 24, 0, 8192, 8192, 0, b});
    assert(missing.description == "minimum" && missing.log_info.at("soft_claim_min_bytes") == "0");
    // Actual R can make even the highest-priority owner prefer/select Valiant.
    // Restart the candidate sequence: a later sample may equal minimal and
    // legitimately provide no alternative corridor at all.
    DragonflySwitch fresh(events, "fresh", DragonflySwitch::GENERAL, 0, 0, &topology);
    auto* original = topology.switches[0];
    topology.switches[0] = &fresh;
    gra.commit({7, 99, 0}, minimal, 1000000);
    const auto high = policy.decide({topology, 0, 24, 0, 8192, 8192, 0, a});
    assert(high.description == "valiant");
    assert(high.log_info.at("soft_claim_min_bytes") == "0");
    assert(high.log_info.at("soft_claim_val_bytes") == "0");
    assert(gra.demandPolicy()->claims().size() == 1);
    assert(gra.demandPolicy()->claims().begin()->first != minimal);
    // Disabled policy has no demand state and uses the same base pressure.
    gra.setDemandPolicy(nullptr);
    DragonflySwitch baseline(events, "baseline", DragonflySwitch::GENERAL, 0, 0, &topology);
    topology.switches[0] = &baseline;
    const auto disabled = policy.decide({topology, 0, 24, 0, 8192, 8192, 0, b});
    assert(disabled.description == "valiant" && disabled.log_info.count("soft_claim_min_bytes") == 0);
    topology.switches[0] = original;
}
}

int main() {
    checkPolicy();
    checkEntryLifecycle();
    checkFallbackAndActualPressure();
    std::cout << "soft claim tests passed\n";
}
