#include "online_fct_predictor.h"

#include <stdexcept>
#include <string>

OnlineFctPredictor::OnlineFctPredictor(const GraStaticGraph& graph) {
    for (const auto& arc : graph.eventArcs()) {
        if (arc.source.node_index != arc.target.node_index ||
            arc.source.event != GraStaticGraph::Event::START ||
            arc.target.event != GraStaticGraph::Event::FINISH) continue;
        const auto& node = graph.dags().at(arc.source.dag_index).nodes.at(arc.source.node_index);
        if (node.type != GraStaticGraph::Type::COMM) continue;
        if (!node.collective_id)
            throw std::invalid_argument("global_slack_online requires collective_id metadata for DAG " +
                std::to_string(arc.source.dag_index) + " flow " + std::to_string(node.flow_id));
        _flows.emplace(Flow{arc.source.dag_index, node.flow_id}, FlowInfo{*node.collective_id, arc.cost_ns});
    }
}

bool OnlineFctPredictor::observeCompletion(Flow flow, uint64_t start_ps, uint64_t end_ps, uint64_t now_ps) {
    const auto& info = _flows.at(flow);
    if (end_ps < start_ps || end_ps > now_ps)
        throw std::invalid_argument("Online FCT observation must be a completed network flow visible now");
    // Convert the duration, not separately rounded start/end timestamps.
    // Visibility uses the estimator's integer-ns clock; sub-ns FCT is retained.
    const Sample sample{now_ps / 1000, static_cast<long double>(end_ps - start_ps) / 1000.0L};
    if (!_completed.emplace(flow, sample).second) return false;
    auto& samples = _history[{flow.first, info.collective_id}];
    samples.push_back(sample);
    if (samples.size() > window_size) samples.pop_front();
    return true;
}

OnlineFctPredictor::Prediction OnlineFctPredictor::predict(Flow flow, uint64_t now_ns) const {
    const auto& info = _flows.at(flow);
    long double sum = 0;
    size_t count = 0;
    const auto found = _history.find({flow.first, info.collective_id});
    if (found != _history.end()) for (const auto& sample : found->second) {
        if (sample.visible_ns > now_ns) continue;
        sum += sample.fct_ns;
        ++count;
    }
    const auto completed = _completed.find(flow);
    if (completed != _completed.end() && completed->second.visible_ns <= now_ns)
        return {info.collective_id, completed->second.fct_ns, count, false};
    return {info.collective_id, count ? sum / count : static_cast<long double>(info.nominal_ns), count, count == 0};
}
