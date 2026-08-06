#pragma once

#include <memory>

#include "opentelemetry/sdk/metrics/push_metric_exporter.h"
#include "otel_metrics/metrics.h"

namespace otel_metrics::testing {

Status InitializeWithExporter(
    const Config &config,
    std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> exporter);

} // namespace otel_metrics::testing
