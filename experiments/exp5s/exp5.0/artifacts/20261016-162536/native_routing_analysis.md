# exp5.0 — Native packet-level UGAL-L

Status: **PASS**

- CCT: **41.799 µs** (41799 ns), maximum GOAL rank graph finish.
- Completed flows / GOAL tasks: **60 / 200**.
- Runtime: **transparent**; runtime decisions / SOURCE labels: **0**.
- Switch routing: **HTSim UGAL_L**, including retransmissions.
- Packet-hop records: **12177**; data packet attempts: **960**.
- Delivered data packets on minimum / detour paths: **666 / 294**.

Workload binary, GOAL, metadata, topology and placement match exp5.1 byte-for-byte. CC, queue, path entropy and cwnd settings are unchanged. All GOAL tasks and transport flows completed; no runtime decision trace or static routing config was used.

`output_metrics/packet_routes.csv` reconstructs actual DATA packet switch paths from the forwarding trace. Minimum/detour classifies each delivered packet's observed hop count on this one-switch-per-group topology; it is not a cached flow label or a count of UGAL comparisons.

The runtime still connects GOAL events to UEC. Its network decision and label cache are bypassed; the original sender, receiver-control and retransmission paths remain active. UGAL-L samples queues at switches and selects routes at packet arrival.

Single deterministic run: simulator seed 13, per-switch UGAL RNG seed 33. Slack analysis is offline and does not influence routing.
