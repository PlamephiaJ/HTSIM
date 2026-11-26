#ifndef GRA_GOAL_GRAPH_H
#define GRA_GOAL_GRAPH_H

#include "flow_routing_config.h"
#include "gra_static_graph.h"

class Parser;

struct GraGoalGraph {
    GraStaticGraph::Dag dag;
    FlowRoutingConfig identities;
};

// Assign one identity/cost to each GOAL SEND/RECV pair, retaining separate
// endpoint starts and typed START/FINISH releases. When identities are absent,
// (src_rank << 32) | send_task_id is
// the stable DAG flow ID and is also used by the messagelet runtime.
GraGoalGraph buildGraGoalGraph(const Parser& parser, uint64_t dag_index,
                              const FlowRoutingConfig* identities = nullptr);

// Retain scheduler rank/task IDs while giving each rank's declared DAG its
// own estimator graph and terminal set. Communication cannot cross the split.
std::vector<GraStaticGraph::Dag> partitionGraGoalDag(GraStaticGraph::Dag combined,
                                                   const std::vector<uint64_t>& rank_dags);

#endif
