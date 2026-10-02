#include "messagelet_config.h"

#include <stdexcept>

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
    readOptional(tree, "messagelet.size_bytes", config.messagelet.size_bytes, filename);
    readOptional(tree, "routing.default", config.routing.default_route, filename);
    readOptional(tree, "logging.trace_decisions", config.logging.trace_decisions, filename);

    if (schema_version != kSchemaVersion) {
        throw std::runtime_error("Unsupported messagelet config schema_version " +
                                 std::to_string(schema_version));
    }

    config.validate();
    return config;
}

void MessageletConfig::validate() const {
    if (routing.default_route != "minimum") {
        throw std::invalid_argument("Unsupported default messagelet route '" +
                                    routing.default_route + "'");
    }
}
