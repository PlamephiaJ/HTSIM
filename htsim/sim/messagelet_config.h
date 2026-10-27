#ifndef MESSAGELET_CONFIG_H
#define MESSAGELET_CONFIG_H

#include <cstdint>
#include <string>
#include <optional>

struct MessageletConfig {
    static constexpr uint32_t kSchemaVersion = 1;

    enum class Granularity { Flow, Messagelet, Packet };
    enum class Mode { Label, Transparent };
    enum class RoutingMode { SOURCE, ENTRY_SWITCH };

    struct RuntimeSettings {
        // Transparent mode bypasses runtime routing and granularity handling.
        Mode mode = Mode::Label;
        Granularity granularity = Granularity::Flow;
        // Must match the Dragonfly switch strategy for ENTRY_SWITCH.
        RoutingMode routing_mode = RoutingMode::SOURCE;
        uint64_t dag_id = 0;
    } runtime;

    struct MessageletSettings {
        // Positive only in messagelet mode. Legacy configs infer flow mode
        // from zero; explicit packet mode uses the transport packet size.
        uint64_t size_bytes = 0;
    } messagelet;

    struct RoutingSettings {
        std::string default_route = "minimum";
        std::string network_decision = "none";
        double commitment_alpha = 1.0;
        struct SoftClaimSettings {
            bool enabled = false;
            std::string policy = "dag_priority";
            std::string priority = "global_slack";
            // Unset follows commitment_alpha, including programmatic changes.
            std::optional<double> beta;
        } soft_claim;
        double softClaimBeta() const { return soft_claim.beta.value_or(commitment_alpha); }
    } routing;

    struct LoggingSettings {
        bool trace_decisions = false;
    } logging;

    static MessageletConfig loadFromFile(const std::string& filename);
    static const char* granularityName(Granularity granularity);
    void validate() const;
};

#endif // MESSAGELET_CONFIG_H
