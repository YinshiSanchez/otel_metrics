#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "opentelemetry/exporters/memory/in_memory_metric_data.h"
#include "opentelemetry/exporters/memory/in_memory_metric_exporter_factory.h"
#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/metrics/data/point_data.h"
#include "otel_metrics/metrics.h"
#include "testing.h"

namespace {

namespace memory_exporter = opentelemetry::exporter::memory;
namespace sdk_metrics = opentelemetry::sdk::metrics;

int failures = 0;

void Check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
  }
}

template <typename T>
T Take(otel_metrics::Result<T> result, const char *message) {
  Check(result.ok(), message);
  if (!result)
    std::exit(2);
  return std::move(result).value();
}

class EnvironmentGuard {
public:
  explicit EnvironmentGuard(const char *name) : name_(name) {
    if (const char *value = std::getenv(name); value != nullptr)
      old_ = value;
  }
  ~EnvironmentGuard() {
    if (old_.has_value()) {
      setenv(name_.c_str(), old_->c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

private:
  std::string name_;
  std::optional<std::string> old_;
};

const sdk_metrics::PointType *OnlyPoint(
    const std::shared_ptr<memory_exporter::SimpleAggregateInMemoryMetricData>
        &data,
    const std::string &metric) {
  const auto &points = data->Get("otel_metrics_test", metric);
  Check(points.size() == 1, ("expected one series for " + metric).c_str());
  return points.empty() ? nullptr : &points.begin()->second;
}

void TestValidation() {
  EnvironmentGuard disabled_guard("OTEL_SDK_DISABLED");
  EnvironmentGuard service_guard("OTEL_SERVICE_NAME");
  EnvironmentGuard resource_guard("OTEL_RESOURCE_ATTRIBUTES");
  EnvironmentGuard protocol_guard("OTEL_EXPORTER_OTLP_METRICS_PROTOCOL");
  unsetenv("OTEL_SERVICE_NAME");
  unsetenv("OTEL_RESOURCE_ATTRIBUTES");
  unsetenv("OTEL_EXPORTER_OTLP_METRICS_PROTOCOL");
  setenv("OTEL_SDK_DISABLED", "false", 1);

  otel_metrics::Config missing_service;
  missing_service.protocol = otel_metrics::Protocol::kHttpProtobuf;
  Check(otel_metrics::Initialize(missing_service).code() ==
            otel_metrics::StatusCode::kInvalidArgument,
        "service.name is required");

  otel_metrics::Config invalid_endpoint;
  invalid_endpoint.service_name = "test";
  invalid_endpoint.protocol = otel_metrics::Protocol::kHttpProtobuf;
  invalid_endpoint.endpoint = "http://localhost:4318";
  Check(otel_metrics::Initialize(invalid_endpoint).code() ==
            otel_metrics::StatusCode::kInvalidArgument,
        "HTTP endpoint path is validated");

  setenv("OTEL_EXPORTER_OTLP_METRICS_PROTOCOL", "http/json", 1);
  otel_metrics::Config invalid_protocol;
  invalid_protocol.service_name = "test";
  Check(otel_metrics::Initialize(invalid_protocol).code() ==
            otel_metrics::StatusCode::kInvalidArgument,
        "unsupported environment protocol is rejected");
}

void TestDisabledRuntime() {
  EnvironmentGuard disabled_guard("OTEL_SDK_DISABLED");
  setenv("OTEL_SDK_DISABLED", "TrUe", 1);

  auto data =
      std::make_shared<memory_exporter::SimpleAggregateInMemoryMetricData>();
  auto exporter = memory_exporter::InMemoryMetricExporterFactory::Create(data);

  otel_metrics::Config config;
  Check(
      otel_metrics::testing::InitializeWithExporter(config, std::move(exporter))
          .ok(),
      "disabled runtime initializes without exporter configuration");
  Check(otel_metrics::IsInitialized(),
        "disabled runtime reports initialized");

  const auto stats = otel_metrics::GetRuntimeStats();
  Check(stats.initialized && stats.disabled,
        "runtime stats report that metrics are disabled");

  auto counter = Take(otel_metrics::CreateCounter<std::uint64_t>(
                          {"disabled.requests", "requests", "{request}"}),
                      "create Counter while metrics are disabled");
  auto histogram = Take(otel_metrics::CreateHistogram<double>(
                            {{"disabled.latency", "latency", "s"}, {0.1, 1.0}}),
                        "create Histogram while metrics are disabled");
  otel_metrics::GaugeOptions gauge_options;
  gauge_options.name = "disabled.temperature";
  auto gauge = Take(otel_metrics::CreateGauge<double>(gauge_options),
                    "create Gauge while metrics are disabled");

  Check(counter.Add(1, {{"route", "/health"}}),
        "disabled Counter accepts a measurement as a no-op");
  Check(histogram.Record(std::numeric_limits<double>::quiet_NaN()),
        "disabled Histogram avoids measurement validation work");
  Check(gauge.Set(42.0, {{"zone", "a"}}),
        "disabled Gauge accepts a measurement as a no-op");
  Check(!gauge.Remove({{"zone", "a"}}) && gauge.Clear() == 0,
        "disabled Gauge does not retain series");
  Check(otel_metrics::ForceFlush(std::chrono::seconds(1)).ok(),
        "disabled runtime force flush is a no-op");

  const auto after_records = otel_metrics::GetRuntimeStats();
  Check(after_records.successful_exports == 0 &&
            after_records.failed_exports == 0 &&
            after_records.dropped_measurements == 0,
        "disabled runtime neither exports nor counts intentional no-ops as drops");

  Check(otel_metrics::Shutdown(std::chrono::seconds(1)).ok(),
        "disabled runtime shuts down");
  Check(!counter.Add(1),
        "disabled runtime handles become inert after shutdown");
}

void TestInstrumentsAndLifecycle() {
  auto data =
      std::make_shared<memory_exporter::SimpleAggregateInMemoryMetricData>();
  auto exporter = memory_exporter::InMemoryMetricExporterFactory::Create(data);

  otel_metrics::Config config;
  config.service_name = "otel-metrics-test";
  config.resource_attributes = {{"deployment.environment", "test"},
                                {"replica", 2}};
  config.instrumentation_scope_name = "otel_metrics_test";
  config.protocol = otel_metrics::Protocol::kHttpProtobuf;
  config.export_interval = std::chrono::hours(1);
  config.default_gauge_series_limit = 2;
  Check(
      otel_metrics::testing::InitializeWithExporter(config, std::move(exporter))
          .ok(),
      "initialize with in-memory exporter");
  Check(otel_metrics::IsInitialized(), "runtime reports initialized");

  auto unused_exporter = memory_exporter::InMemoryMetricExporterFactory::Create(
      std::make_shared<memory_exporter::SimpleAggregateInMemoryMetricData>());
  Check(otel_metrics::testing::InitializeWithExporter(
            config, std::move(unused_exporter))
            .ok(),
        "same effective config is idempotent");
  auto changed = config;
  changed.service_name = "different";
  auto changed_exporter =
      memory_exporter::InMemoryMetricExporterFactory::Create(
          std::make_shared<
              memory_exporter::SimpleAggregateInMemoryMetricData>());
  Check(otel_metrics::testing::InitializeWithExporter(
            changed, std::move(changed_exporter))
                .code() == otel_metrics::StatusCode::kAlreadyInitialized,
        "different effective config is rejected");

  auto counter = Take(otel_metrics::CreateCounter<std::uint64_t>(
                          {"test.requests", "requests", "{request}"}),
                      "create uint64 Counter");
  auto counter_again = Take(otel_metrics::CreateCounter<std::uint64_t>(
                                {"TEST.REQUESTS", "requests", "{request}"}),
                            "identical Counter is reused case-insensitively");
  Check(otel_metrics::CreateCounter<double>(
            {"test.requests", "requests", "{request}"})
                .status()
                .code() == otel_metrics::StatusCode::kInstrumentConflict,
        "same name with another number type conflicts");

  auto up_down = Take(otel_metrics::CreateUpDownCounter<double>(
                          {"test.queue", "queue delta", "{item}"}),
                      "create double UpDownCounter");
  otel_metrics::HistogramOptions histogram_options;
  histogram_options.name = "test.latency";
  histogram_options.description = "latency";
  histogram_options.unit = "s";
  histogram_options.boundaries = {0.1, 0.5, 1.0};
  auto histogram =
      Take(otel_metrics::CreateHistogram<double>(histogram_options),
           "create double Histogram");
  auto invalid_histogram = histogram_options;
  invalid_histogram.name = "test.invalid_histogram";
  invalid_histogram.boundaries = {1.0, 0.5};
  Check(otel_metrics::CreateHistogram<double>(invalid_histogram)
                .status()
                .code() == otel_metrics::StatusCode::kInvalidArgument,
        "Histogram boundaries are validated");

  otel_metrics::GaugeOptions gauge_options;
  gauge_options.name = "test.temperature";
  gauge_options.description = "latest temperature";
  gauge_options.unit = "Cel";
  gauge_options.series_limit = 2;
  auto gauge = Take(otel_metrics::CreateGauge<double>(gauge_options),
                    "create double Gauge");
  auto concurrent =
      Take(otel_metrics::CreateCounter<std::uint64_t>(
               {"test.concurrent", "concurrent records", "{record}"}),
           "create concurrent Counter");

  const otel_metrics::Attributes all_types = {
      {"string", "value"}, {"boolean", true}, {"integer", 42}, {"double", 2.5}};
  Check(counter.Add(1, all_types) && counter_again.Add(2, all_types),
        "Counter records through reused handles");
  Check(up_down.Add(3.5, all_types) && up_down.Add(-1.0, all_types),
        "UpDownCounter accepts positive and negative values");
  Check(histogram.Record(0.05, all_types) && histogram.Record(0.75, all_types),
        "Histogram records values");
  Check(!histogram.Record(std::numeric_limits<double>::quiet_NaN(), all_types),
        "non-finite measurement is rejected");

  Check(gauge.Set(10.0, {{"zone", "a"}, {"rack", 1}}),
        "Gauge creates first series");
  Check(gauge.Set(20.0, {{"zone", "b"}}), "Gauge creates second series");
  Check(!gauge.Set(30.0, {{"zone", "c"}}),
        "Gauge rejects a series above its limit");
  Check(gauge.Set(11.0, {{"rack", 1}, {"zone", "a"}}),
        "Gauge updates an existing series independent of attribute order");
  Check(gauge.Remove({{"zone", "b"}}), "Gauge removes a series");
  Check(!gauge.Set(1.0, {{"duplicate", 1}, {"duplicate", 2}}),
        "duplicate attribute keys are rejected");

  std::vector<std::thread> writers;
  for (int thread = 0; thread < 4; ++thread) {
    writers.emplace_back([concurrent] {
      for (int i = 0; i < 250; ++i)
        concurrent.Add(1);
    });
  }
  for (auto &writer : writers)
    writer.join();

  Check(otel_metrics::ForceFlush(std::chrono::seconds(2)).ok(),
        "force flush succeeds");
  auto stats = otel_metrics::GetRuntimeStats();
  Check(stats.successful_exports >= 1, "successful exports are counted");
  Check(stats.dropped_measurements >= 2, "invalid measurements are counted");
  Check(stats.rejected_gauge_series == 1,
        "Gauge series rejections are counted");

  if (const auto *point = OnlyPoint(data, "test.requests")) {
    const auto *sum =
        opentelemetry::nostd::get_if<sdk_metrics::SumPointData>(point);
    Check(sum != nullptr, "Counter exports SumPointData");
    if (sum) {
      const auto *value =
          opentelemetry::nostd::get_if<std::int64_t>(&sum->value_);
      Check(value != nullptr && *value == 3,
            "Counter export contains accumulated value");
    }
  }
  if (const auto *point = OnlyPoint(data, "test.latency")) {
    const auto *hist =
        opentelemetry::nostd::get_if<sdk_metrics::HistogramPointData>(point);
    Check(hist != nullptr, "Histogram exports HistogramPointData");
    if (hist) {
      Check(hist->count_ == 2, "Histogram export contains count");
      Check(hist->boundaries_ == histogram_options.boundaries,
            "Histogram export contains explicit boundaries");
    }
  }
  if (const auto *point = OnlyPoint(data, "test.temperature")) {
    const auto *last =
        opentelemetry::nostd::get_if<sdk_metrics::LastValuePointData>(point);
    Check(last != nullptr, "Gauge exports LastValuePointData");
    if (last) {
      const auto *value = opentelemetry::nostd::get_if<double>(&last->value_);
      Check(value != nullptr && std::abs(*value - 11.0) < 1e-12,
            "Gauge exports the latest value");
    }
  }
  if (const auto *point = OnlyPoint(data, "test.concurrent")) {
    const auto *sum =
        opentelemetry::nostd::get_if<sdk_metrics::SumPointData>(point);
    Check(sum != nullptr, "concurrent Counter exports SumPointData");
    if (sum) {
      const auto *value =
          opentelemetry::nostd::get_if<std::int64_t>(&sum->value_);
      Check(value != nullptr && *value == 1000,
            "concurrent Counter contains every recorded value");
    }
  }

  otel_metrics::Status shutdown_status;
  std::thread shutdown_thread([&] {
    shutdown_status = otel_metrics::Shutdown(std::chrono::seconds(2));
  });
  for (int i = 0; i < 1000; ++i)
    counter.Add(1);
  shutdown_thread.join();
  Check(shutdown_status.ok(), "shutdown succeeds while records are in flight");
  Check(otel_metrics::Shutdown(std::chrono::seconds(2)).ok(),
        "shutdown is idempotent");
  Check(!otel_metrics::IsInitialized(), "runtime reports shutdown");
  Check(!counter.Add(1), "old handle becomes inert after shutdown");

  auto new_data =
      std::make_shared<memory_exporter::SimpleAggregateInMemoryMetricData>();
  auto new_exporter =
      memory_exporter::InMemoryMetricExporterFactory::Create(new_data);
  Check(otel_metrics::testing::InitializeWithExporter(config,
                                                      std::move(new_exporter))
            .ok(),
        "runtime can initialize again after shutdown");
  Check(otel_metrics::Shutdown(std::chrono::seconds(2)).ok(),
        "reinitialized runtime shuts down");
}

} // namespace

int main() {
  EnvironmentGuard disabled_guard("OTEL_SDK_DISABLED");
  unsetenv("OTEL_SDK_DISABLED");
  TestValidation();
  TestDisabledRuntime();
  TestInstrumentsAndLifecycle();
  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return 1;
  }
  std::cout << "otel_metrics tests passed\n";
  return 0;
}
