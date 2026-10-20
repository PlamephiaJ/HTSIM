#include "messagelet_config.h"

#include <stdexcept>
#include <cmath>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace {
template <typename T>
void readOptional(const boost::property_tree::ptree& tree,
                  const std::string& key,
                  T& destination,
                  const std::string& filename) {
    auto value = tree.get_child_optional(key);
    if (!value) {
        return;
    }

    try {
        destination = value->get_value<T>();
    } catch (const boost::property_tree::ptree_error&) {
        throw std::runtime_error("Invalid value for '" + key +
                                 "' in messagelet config '" + filename + "'");
    }
}
} // namespace

MessageletConfig MessageletConfig::loadFromFile(const std::string& filename) {
    boost::property_tree::ptree tree;
    try {
        boost::property_tree::read_json(filename, tree);
    } catch (const boost::property_tree::json_parser_error& error) {
        throw std::runtime_error("Failed to parse messagelet config '" + filename +
                                 "': " + error.message());
    }

    uint32_t schema_version = kSchemaVersion;
    MessageletConfig config;
    readOptional(tree, "schema_version", schema_version, filename);
    std::string mode = "label";
    readOptional(tree, "runtime.mode", mode, filename);
    if (mode == "label") {
        config.runtime.mode = Mode::Label;
    } else if (mode == "transparent") {
        config.runtime.mode = Mode::Transparent;
    } else {
        throw std::invalid_argument("Unsupported runtime mode '" + mode + "'");
    }
    readOptional(tree, "messagelet.size_bytes", config.messagelet.size_bytes, filename);
    // Legacy configs select granularity through size_bytes alone.
    std::string granularity = config.messagelet.size_bytes == 0 ? "flow" : "messagelet";
    readOptional(tree, "runtime.granularity", granularity, filename);
    if (granularity == "flow") {
        config.runtime.granularity = Granularity::Flow;
    } else if (granularity == "messagelet") {
        config.runtime.granularity = Granularity::Messagelet;
    } else if (granularity == "packet") {
        config.runtime.granularity = Granularity::Packet;
    } else {
        throw std::invalid_argument("Unsupported runtime granularity '" + granularity + "'");
    }
    readOptional(tree, "routing.default", config.routing.default_route, filename);
    readOptional(tree, "routing.network_decision", config.routing.network_decision, filename);
    readOptional(tree, "routing.commitment_alpha", config.routing.commitment_alpha, filename);
    readOptional(tree, "logging.trace_decisions", config.logging.trace_decisions, filename);

    if (schema_version != kSchemaVersion) {
        throw std::runtime_error("Unsupported messagelet config schema_version " +
                                 std::to_string(schema_version));
    }

    config.validate();
    return config;
}

void MessageletConfig::validate() const {
    if (runtime.mode != Mode::Label && runtime.mode != Mode::Transparent) {
        throw std::invalid_argument("Unsupported runtime mode");
    }
    if (runtime.mode == Mode::Transparent && routing.network_decision != "none") {
        throw std::invalid_argument("runtime.mode 'transparent' requires routing.network_decision = 'none'");
    }
    granularityName(runtime.granularity);
    if (runtime.granularity == Granularity::Flow && messagelet.size_bytes != 0) {
        throw std::invalid_argument("runtime.granularity 'flow' requires messagelet.size_bytes = 0");
    }
    if (runtime.granularity == Granularity::Messagelet && messagelet.size_bytes == 0) {
        throw std::invalid_argument("runtime.granularity 'messagelet' requires messagelet.size_bytes > 0");
    }
    if (runtime.granularity == Granularity::Packet && messagelet.size_bytes != 0) {
        throw std::invalid_argument("runtime.granularity 'packet' requires messagelet.size_bytes = 0");
    }
    if (routing.network_decision != "none" && routing.network_decision != "ugal_l" &&
        routing.network_decision != "commitment_aware_ugal") {
        throw std::invalid_argument("Unsupported network decision strategy '" +
                                    routing.network_decision + "'");
    }
    if (!std::isfinite(routing.commitment_alpha) || routing.commitment_alpha < 0) {
        throw std::invalid_argument("routing.commitment_alpha must be finite and nonnegative");
    }
    if (routing.default_route != "minimum") {
        throw std::invalid_argument("Unsupported default messagelet route '" +
                                    routing.default_route + "'");
    }
}

const char* MessageletConfig::granularityName(Granularity granularity) {
    switch (granularity) {
        case Granularity::Flow: return "flow";
        case Granularity::Messagelet: return "messagelet";
        case Granularity::Packet: return "packet";
    }
    throw std::invalid_argument("Unsupported runtime granularity");
}
