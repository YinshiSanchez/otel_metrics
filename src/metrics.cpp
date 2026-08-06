#include "otel_metrics/metrics.h"
#include "testing.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <tuple>
#include <unordered_map>

#include "opentelemetry/common/attribute_value.h"
#include "opentelemetry/common/key_value_iterable_view.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_metric_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_metric_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h"
#include "opentelemetry/metrics/async_instruments.h"
#include "opentelemetry/metrics/meter.h"
#include "opentelemetry/metrics/observer_result.h"
#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/common/disabled.h"
#include "opentelemetry/sdk/common/exporter_utils.h"
#include "opentelemetry/sdk/metrics/aggregation/aggregation_config.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h"
#include "opentelemetry/sdk/metrics/meter_context_factory.h"
#include "opentelemetry/sdk/metrics/meter_provider.h"
#include "opentelemetry/sdk/metrics/meter_provider_factory.h"
#include "opentelemetry/sdk/metrics/push_metric_exporter.h"
#include "opentelemetry/sdk/metrics/view/instrument_selector_factory.h"
#include "opentelemetry/sdk/metrics/view/meter_selector_factory.h"
#include "opentelemetry/sdk/metrics/view/view_factory.h"
#include "opentelemetry/sdk/metrics/view/view_registry.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/sdk/resource/resource_detector.h"

namespace otel_metrics {
namespace {

namespace api_metrics = opentelemetry::metrics;
namespace otlp = opentelemetry::exporter::otlp;
namespace sdk_common = opentelemetry::sdk::common;
namespace sdk_metrics = opentelemetry::sdk::metrics;
namespace sdk_resource = opentelemetry::sdk::resource;

struct AtomicStats {
  std::atomic<std::uint64_t> successful_exports{0};
  std::atomic<std::uint64_t> failed_exports{0};
  std::atomic<std::uint64_t> dropped_measurements{0};
  std::atomic<std::uint64_t> rejected_gauge_series{0};
};

struct EffectiveConfig {
  bool disabled = false;
  Protocol protocol = Protocol::kHttpProtobuf;
  otlp::OtlpHttpMetricExporterOptions http_options;
  otlp::OtlpGrpcMetricExporterOptions grpc_options;
  sdk_metrics::PeriodicExportingMetricReaderOptions reader_options;
  sdk_resource::Resource resource;
  std::string scope_name;
  std::string scope_version;
  std::size_t default_gauge_series_limit = 1024;
  std::string signature;
};

struct InstrumentDescriptor {
  detail::InstrumentKind kind;
  detail::NumberKind number_kind;
  std::string name;
  std::string description;
  std::string unit;
  std::vector<double> boundaries;
  std::optional<std::size_t> gauge_series_limit;

  bool SameDefinition(const InstrumentDescriptor &other) const {
    return kind == other.kind && number_kind == other.number_kind &&
           description == other.description && unit == other.unit &&
           boundaries == other.boundaries &&
           gauge_series_limit == other.gauge_series_limit;
  }
};

struct RuntimeState {
  mutable std::shared_mutex activity_mutex;
  bool active = true;
  std::mutex registry_mutex;
  std::unordered_map<
      std::string,
      std::pair<InstrumentDescriptor, std::shared_ptr<detail::Instrument>>>
      instruments;
  std::unique_ptr<sdk_metrics::MeterProvider> provider;
  opentelemetry::nostd::shared_ptr<api_metrics::Meter> meter;
  std::shared_ptr<AtomicStats> stats;
  EffectiveConfig config;
};

std::mutex g_runtime_mutex;
std::shared_ptr<RuntimeState> g_runtime;
std::shared_ptr<AtomicStats> g_last_stats;

std::string LowerAscii(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool IsFinite(double value) { return std::isfinite(value); }

bool ValidateAttributes(const Attributes &attributes) {
  for (std::size_t i = 0; i < attributes.size(); ++i) {
    if (attributes[i].key.empty())
      return false;
    if (const auto *number = std::get_if<double>(&attributes[i].value);
        number != nullptr && !IsFinite(*number)) {
      return false;
    }
    for (std::size_t j = i + 1; j < attributes.size(); ++j) {
      if (attributes[i].key == attributes[j].key)
        return false;
    }
  }
  return true;
}

bool AttributeLess(const Attribute &left, const Attribute &right) {
  if (left.key != right.key)
    return left.key < right.key;
  return left.value < right.value;
}

bool CanonicalizeAttributes(const Attributes &input, Attributes &output) {
  if (!ValidateAttributes(input))
    return false;
  output = input;
  std::sort(output.begin(), output.end(), AttributeLess);
  return true;
}

struct AttributesLess {
  bool operator()(const Attributes &left, const Attributes &right) const {
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(),
                                        right.end(), AttributeLess);
  }
};

using OtelAttribute =
    std::pair<std::string, opentelemetry::common::AttributeValue>;
using OtelAttributes = std::vector<OtelAttribute>;

OtelAttributes ToOtelAttributes(const Attributes &attributes) {
  OtelAttributes converted;
  converted.reserve(attributes.size());
  for (const auto &attribute : attributes) {
    std::visit(
        [&](const auto &value) {
          using Value = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Value, std::string>) {
            converted.emplace_back(
                attribute.key,
                opentelemetry::nostd::string_view(value.data(), value.size()));
          } else {
            converted.emplace_back(attribute.key, value);
          }
        },
        attribute.value);
  }
  return converted;
}

sdk_resource::ResourceAttributes
ToResourceAttributes(const Attributes &attributes) {
  sdk_resource::ResourceAttributes converted;
  for (const auto &attribute : attributes) {
    std::visit([&](const auto &value) { converted[attribute.key] = value; },
               attribute.value);
  }
  return converted;
}

template <typename T>
void AppendSignatureValue(std::ostringstream &stream, const T &value) {
  stream << value;
}

void AppendAttributesSignature(std::ostringstream &stream,
                               Attributes attributes) {
  std::sort(attributes.begin(), attributes.end(), AttributeLess);
  for (const auto &attribute : attributes) {
    stream << attribute.key.size() << ':' << attribute.key << '='
           << attribute.value.index() << ':';
    std::visit([&](const auto &value) { AppendSignatureValue(stream, value); },
               attribute.value);
    stream << ';';
  }
}

template <typename Duration> long long DurationMicros(Duration value) {
  return std::chrono::duration_cast<std::chrono::microseconds>(value).count();
}

void AppendHeaders(std::ostringstream &stream,
                   const otlp::OtlpHeaders &headers) {
  for (const auto &header : headers) {
    stream << header.first.size() << ':' << header.first << '='
           << header.second.size() << ':' << header.second << ';';
  }
}

std::string
HttpOptionsSignature(const otlp::OtlpHttpMetricExporterOptions &options) {
  std::ostringstream stream;
  stream << options.url << '|' << DurationMicros(options.timeout) << '|'
         << options.ssl_insecure_skip_verify << '|' << options.ssl_ca_cert_path
         << '|' << options.ssl_ca_cert_string << '|'
         << options.ssl_client_key_path << '|' << options.ssl_client_key_string
         << '|' << options.ssl_client_cert_path << '|'
         << options.ssl_client_cert_string << '|' << options.ssl_min_tls << '|'
         << options.ssl_max_tls << '|' << options.ssl_cipher << '|'
         << options.ssl_cipher_suite << '|' << options.compression << '|'
         << static_cast<int>(options.aggregation_temporality) << '|'
         << options.retry_policy_max_attempts << '|'
         << options.retry_policy_initial_backoff.count() << '|'
         << options.retry_policy_max_backoff.count() << '|'
         << options.retry_policy_backoff_multiplier << '|';
  AppendHeaders(stream, options.http_headers);
  return stream.str();
}

std::string
GrpcOptionsSignature(const otlp::OtlpGrpcMetricExporterOptions &options) {
  std::ostringstream stream;
  stream << options.endpoint << '|' << options.use_ssl_credentials << '|'
         << options.ssl_credentials_cacert_path << '|'
         << options.ssl_credentials_cacert_as_string << '|'
         << DurationMicros(options.timeout) << '|' << options.user_agent << '|'
         << options.max_threads << '|' << options.compression << '|'
         << static_cast<int>(options.aggregation_temporality) << '|'
         << options.retry_policy_max_attempts << '|'
         << options.retry_policy_initial_backoff.count() << '|'
         << options.retry_policy_max_backoff.count() << '|'
         << options.retry_policy_backoff_multiplier << '|';
  AppendHeaders(stream, options.metadata);
  return stream.str();
}

std::optional<std::string> Environment(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr)
    return std::nullopt;
  return std::string(value);
}

Result<Protocol> ResolveProtocol(const Config &config) {
  if (config.protocol.has_value())
    return Result<Protocol>::Success(*config.protocol);
  auto value = Environment("OTEL_EXPORTER_OTLP_METRICS_PROTOCOL");
  if (!value.has_value())
    value = Environment("OTEL_EXPORTER_OTLP_PROTOCOL");
  if (!value.has_value() || *value == "http/protobuf") {
    return Result<Protocol>::Success(Protocol::kHttpProtobuf);
  }
  if (*value == "grpc")
    return Result<Protocol>::Success(Protocol::kGrpc);
  return Result<Protocol>::Failure(
      {StatusCode::kInvalidArgument,
       "unsupported OTLP metrics protocol: " + *value});
}

bool ValidHttpEndpoint(const std::string &endpoint) {
  const bool scheme =
      endpoint.rfind("http://", 0) == 0 || endpoint.rfind("https://", 0) == 0;
  constexpr std::string_view suffix = "/v1/metrics";
  return scheme && endpoint.size() >= suffix.size() &&
         endpoint.compare(endpoint.size() - suffix.size(), suffix.size(),
                          suffix) == 0;
}

bool ValidGrpcEndpoint(const std::string &endpoint) {
  return (endpoint.rfind("http://", 0) == 0 ||
          endpoint.rfind("https://", 0) == 0) &&
         endpoint.find(':', endpoint.find("//") + 2) != std::string::npos;
}

Status ValidateDurations(const Config &config) {
  const auto positive = [](const auto &value) {
    return !value || value->count() > 0;
  };
  if (!positive(config.export_interval) || !positive(config.export_timeout) ||
      !positive(config.request_timeout)) {
    return {StatusCode::kInvalidArgument,
            "timeouts and export interval must be positive"};
  }
  if (config.default_gauge_series_limit == 0) {
    return {StatusCode::kInvalidArgument,
            "default Gauge series limit must be positive"};
  }
  return Status::Ok();
}

Result<EffectiveConfig> ResolveConfig(const Config &config) {
  EffectiveConfig effective;
  if (sdk_common::GetSdkDisabled()) {
    effective.disabled = true;
    effective.default_gauge_series_limit =
        std::max<std::size_t>(1, config.default_gauge_series_limit);
    effective.signature = "disabled";
    return Result<EffectiveConfig>::Success(std::move(effective));
  }

  if (!ValidateAttributes(config.resource_attributes)) {
    return Result<EffectiveConfig>::Failure(
        {StatusCode::kInvalidArgument,
         "resource attributes contain an empty or duplicate key"});
  }
  auto duration_status = ValidateDurations(config);
  if (!duration_status)
    return Result<EffectiveConfig>::Failure(std::move(duration_status));
  if (config.instrumentation_scope_name.empty()) {
    return Result<EffectiveConfig>::Failure(
        {StatusCode::kInvalidArgument,
         "instrumentation scope name must not be empty"});
  }

  auto protocol = ResolveProtocol(config);
  if (!protocol)
    return Result<EffectiveConfig>::Failure(protocol.status());

  effective.protocol = protocol.value();
  effective.scope_name = config.instrumentation_scope_name;
  effective.scope_version = config.instrumentation_scope_version;
  effective.default_gauge_series_limit = config.default_gauge_series_limit;
  if (config.export_interval) {
    effective.reader_options.export_interval_millis = *config.export_interval;
  }
  if (config.export_timeout) {
    effective.reader_options.export_timeout_millis = *config.export_timeout;
  }

  if (effective.protocol == Protocol::kHttpProtobuf) {
    if (config.endpoint)
      effective.http_options.url = *config.endpoint;
    if (config.request_timeout)
      effective.http_options.timeout = *config.request_timeout;
    effective.http_options.content_type = otlp::HttpRequestContentType::kBinary;
    if (!ValidHttpEndpoint(effective.http_options.url)) {
      return Result<EffectiveConfig>::Failure(
          {StatusCode::kInvalidArgument,
           "OTLP/HTTP metrics endpoint must be an http(s) URL ending in "
           "/v1/metrics"});
    }
  } else {
    if (config.endpoint)
      effective.grpc_options.endpoint = *config.endpoint;
    if (config.request_timeout)
      effective.grpc_options.timeout = *config.request_timeout;
    if (!ValidGrpcEndpoint(effective.grpc_options.endpoint)) {
      return Result<EffectiveConfig>::Failure(
          {StatusCode::kInvalidArgument,
           "OTLP/gRPC metrics endpoint must be an http(s) URI with a port"});
    }
  }

  Attributes explicit_resource_attributes = config.resource_attributes;
  if (!config.service_name.empty()) {
    explicit_resource_attributes.erase(
        std::remove_if(explicit_resource_attributes.begin(),
                       explicit_resource_attributes.end(),
                       [](const Attribute &attribute) {
                         return attribute.key == "service.name";
                       }),
        explicit_resource_attributes.end());
    explicit_resource_attributes.emplace_back("service.name",
                                              config.service_name);
  }

  const auto environment_resource =
      sdk_resource::OTELResourceDetector().Detect();
  auto resource = sdk_resource::Resource::GetDefault()
                      .Merge(environment_resource)
                      .Merge(sdk_resource::Resource(
                          ToResourceAttributes(explicit_resource_attributes)));
  const auto service = resource.GetAttributes().find("service.name");
  if (service == resource.GetAttributes().end()) {
    return Result<EffectiveConfig>::Failure(
        {StatusCode::kInvalidArgument,
         "service.name is required in Config, OTEL_SERVICE_NAME, or resource "
         "attributes"});
  }
  const auto *service_name =
      opentelemetry::nostd::get_if<std::string>(&service->second);
  if (service_name == nullptr || service_name->empty()) {
    return Result<EffectiveConfig>::Failure(
        {StatusCode::kInvalidArgument,
         "service.name must be a non-empty string"});
  }
  effective.resource = std::move(resource);

  std::ostringstream signature;
  signature << static_cast<int>(effective.protocol) << '|'
            << effective.scope_name << '|' << effective.scope_version << '|'
            << effective.default_gauge_series_limit << '|'
            << effective.reader_options.export_interval_millis.count() << '|'
            << effective.reader_options.export_timeout_millis.count() << '|';
  if (effective.protocol == Protocol::kHttpProtobuf) {
    signature << HttpOptionsSignature(effective.http_options);
  } else {
    signature << GrpcOptionsSignature(effective.grpc_options);
  }
  signature << '|';
  Attributes signature_attributes;
  signature_attributes.reserve(effective.resource.GetAttributes().size());
  for (const auto &entry : effective.resource.GetAttributes()) {
    if (const auto *value =
            opentelemetry::nostd::get_if<std::string>(&entry.second)) {
      signature_attributes.emplace_back(entry.first, *value);
    } else if (const auto *value =
                   opentelemetry::nostd::get_if<bool>(&entry.second)) {
      signature_attributes.emplace_back(entry.first, *value);
    } else if (const auto *value =
                   opentelemetry::nostd::get_if<std::int64_t>(&entry.second)) {
      signature_attributes.emplace_back(entry.first, *value);
    } else if (const auto *value =
                   opentelemetry::nostd::get_if<double>(&entry.second)) {
      signature_attributes.emplace_back(entry.first, *value);
    }
  }
  AppendAttributesSignature(signature, std::move(signature_attributes));
  effective.signature = signature.str();
  return Result<EffectiveConfig>::Success(std::move(effective));
}

class StatsExporter final : public sdk_metrics::PushMetricExporter {
public:
  StatsExporter(std::unique_ptr<sdk_metrics::PushMetricExporter> exporter,
                std::shared_ptr<AtomicStats> stats)
      : exporter_(std::move(exporter)), stats_(std::move(stats)) {}

  sdk_common::ExportResult
  Export(const sdk_metrics::ResourceMetrics &data) noexcept override {
    const auto result = exporter_->Export(data);
    if (result == sdk_common::ExportResult::kSuccess) {
      stats_->successful_exports.fetch_add(1, std::memory_order_relaxed);
    } else {
      stats_->failed_exports.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
  }

  sdk_metrics::AggregationTemporality GetAggregationTemporality(
      sdk_metrics::InstrumentType type) const noexcept override {
    return exporter_->GetAggregationTemporality(type);
  }

  bool ForceFlush(std::chrono::microseconds timeout) noexcept override {
    return exporter_->ForceFlush(timeout);
  }

  bool Shutdown(std::chrono::microseconds timeout) noexcept override {
    return exporter_->Shutdown(timeout);
  }

private:
  std::unique_ptr<sdk_metrics::PushMetricExporter> exporter_;
  std::shared_ptr<AtomicStats> stats_;
};

void CountDrop(const std::shared_ptr<AtomicStats> &stats) noexcept {
  stats->dropped_measurements.fetch_add(1, std::memory_order_relaxed);
}

template <typename Callback>
bool WithActiveState(const std::weak_ptr<RuntimeState> &weak_state,
                     const std::shared_ptr<AtomicStats> &stats,
                     Callback &&callback) noexcept {
  try {
    auto state = weak_state.lock();
    if (!state) {
      CountDrop(stats);
      return false;
    }
    std::shared_lock lock(state->activity_mutex);
    if (!state->active) {
      CountDrop(stats);
      return false;
    }
    return callback();
  } catch (...) {
    CountDrop(stats);
    return false;
  }
}

class NoopInstrument final : public detail::Instrument {
public:
  NoopInstrument(std::weak_ptr<RuntimeState> state,
                 std::shared_ptr<AtomicStats> stats)
      : state_(std::move(state)), stats_(std::move(stats)) {}

  bool RecordUInt64(std::uint64_t,
                    const Attributes &) noexcept override {
    return Record();
  }
  bool RecordInt64(std::int64_t, const Attributes &) noexcept override {
    return Record();
  }
  bool RecordDouble(double, const Attributes &) noexcept override {
    return Record();
  }

private:
  bool Record() noexcept {
    return WithActiveState(state_, stats_, [] { return true; });
  }

  std::weak_ptr<RuntimeState> state_;
  std::shared_ptr<AtomicStats> stats_;
};

template <typename T>
class CounterInstrument final : public detail::Instrument {
public:
  CounterInstrument(
      std::weak_ptr<RuntimeState> state, std::shared_ptr<AtomicStats> stats,
      opentelemetry::nostd::unique_ptr<api_metrics::Counter<T>> instrument)
      : state_(std::move(state)), stats_(std::move(stats)),
        instrument_(std::move(instrument)) {}

  bool RecordUInt64(std::uint64_t value,
                    const Attributes &attributes) noexcept override {
    if constexpr (!std::is_same_v<T, std::uint64_t>)
      return false;
    return Record(value, attributes);
  }
  bool RecordDouble(double value,
                    const Attributes &attributes) noexcept override {
    if constexpr (!std::is_same_v<T, double>)
      return false;
    if (!IsFinite(value) || value < 0) {
      CountDrop(stats_);
      return false;
    }
    return Record(value, attributes);
  }

private:
  template <typename U>
  bool Record(U value, const Attributes &attributes) noexcept {
    if (!ValidateAttributes(attributes)) {
      CountDrop(stats_);
      return false;
    }
    return WithActiveState(state_, stats_, [&] {
      auto converted = ToOtelAttributes(attributes);
      instrument_->Add(static_cast<T>(value), converted);
      return true;
    });
  }

  std::weak_ptr<RuntimeState> state_;
  std::shared_ptr<AtomicStats> stats_;
  opentelemetry::nostd::unique_ptr<api_metrics::Counter<T>> instrument_;
};

template <typename T>
class UpDownCounterInstrument final : public detail::Instrument {
public:
  UpDownCounterInstrument(
      std::weak_ptr<RuntimeState> state, std::shared_ptr<AtomicStats> stats,
      opentelemetry::nostd::unique_ptr<api_metrics::UpDownCounter<T>>
          instrument)
      : state_(std::move(state)), stats_(std::move(stats)),
        instrument_(std::move(instrument)) {}

  bool RecordInt64(std::int64_t value,
                   const Attributes &attributes) noexcept override {
    if constexpr (!std::is_same_v<T, std::int64_t>)
      return false;
    return Record(value, attributes);
  }
  bool RecordDouble(double value,
                    const Attributes &attributes) noexcept override {
    if constexpr (!std::is_same_v<T, double>)
      return false;
    if (!IsFinite(value)) {
      CountDrop(stats_);
      return false;
    }
    return Record(value, attributes);
  }

private:
  template <typename U>
  bool Record(U value, const Attributes &attributes) noexcept {
    if (!ValidateAttributes(attributes)) {
      CountDrop(stats_);
      return false;
    }
    return WithActiveState(state_, stats_, [&] {
      auto converted = ToOtelAttributes(attributes);
      instrument_->Add(static_cast<T>(value), converted);
      return true;
    });
  }

  std::weak_ptr<RuntimeState> state_;
  std::shared_ptr<AtomicStats> stats_;
  opentelemetry::nostd::unique_ptr<api_metrics::UpDownCounter<T>> instrument_;
};

template <typename T>
class HistogramInstrument final : public detail::Instrument {
public:
  HistogramInstrument(
      std::weak_ptr<RuntimeState> state, std::shared_ptr<AtomicStats> stats,
      opentelemetry::nostd::unique_ptr<api_metrics::Histogram<T>> instrument)
      : state_(std::move(state)), stats_(std::move(stats)),
        instrument_(std::move(instrument)) {}

  bool RecordUInt64(std::uint64_t value,
                    const Attributes &attributes) noexcept override {
    if constexpr (!std::is_same_v<T, std::uint64_t>)
      return false;
    return RecordValue(value, attributes);
  }
  bool RecordDouble(double value,
                    const Attributes &attributes) noexcept override {
    if constexpr (!std::is_same_v<T, double>)
      return false;
    if (!IsFinite(value) || value < 0) {
      CountDrop(stats_);
      return false;
    }
    return RecordValue(value, attributes);
  }

private:
  template <typename U>
  bool RecordValue(U value, const Attributes &attributes) noexcept {
    if (!ValidateAttributes(attributes)) {
      CountDrop(stats_);
      return false;
    }
    return WithActiveState(state_, stats_, [&] {
      auto converted = ToOtelAttributes(attributes);
      const opentelemetry::context::Context context;
      instrument_->Record(static_cast<T>(value), converted, context);
      return true;
    });
  }

  std::weak_ptr<RuntimeState> state_;
  std::shared_ptr<AtomicStats> stats_;
  opentelemetry::nostd::unique_ptr<api_metrics::Histogram<T>> instrument_;
};

template <typename T> class GaugeInstrument final : public detail::Instrument {
public:
  GaugeInstrument(
      std::weak_ptr<RuntimeState> state, std::shared_ptr<AtomicStats> stats,
      opentelemetry::nostd::shared_ptr<api_metrics::ObservableInstrument>
          instrument,
      std::size_t series_limit)
      : state_(std::move(state)), stats_(std::move(stats)),
        instrument_(std::move(instrument)), series_limit_(series_limit) {
    instrument_->AddCallback(&GaugeInstrument::Observe, this);
  }

  ~GaugeInstrument() override {
    instrument_->RemoveCallback(&GaugeInstrument::Observe, this);
  }

  bool RecordInt64(std::int64_t value,
                   const Attributes &attributes) noexcept override {
    if constexpr (!std::is_same_v<T, std::int64_t>)
      return false;
    return Set(value, attributes);
  }
  bool RecordDouble(double value,
                    const Attributes &attributes) noexcept override {
    if constexpr (!std::is_same_v<T, double>)
      return false;
    if (!IsFinite(value)) {
      CountDrop(stats_);
      return false;
    }
    return Set(value, attributes);
  }

  bool Remove(const Attributes &attributes) noexcept override {
    Attributes canonical;
    if (!CanonicalizeAttributes(attributes, canonical)) {
      CountDrop(stats_);
      return false;
    }
    return WithActiveState(state_, stats_, [&] {
      std::lock_guard lock(values_mutex_);
      return values_.erase(canonical) != 0;
    });
  }

  std::size_t Clear() noexcept override {
    std::size_t removed = 0;
    const bool active = WithActiveState(state_, stats_, [&] {
      std::lock_guard lock(values_mutex_);
      removed = values_.size();
      values_.clear();
      return true;
    });
    return active ? removed : 0;
  }

private:
  template <typename U>
  bool Set(U value, const Attributes &attributes) noexcept {
    Attributes canonical;
    if (!CanonicalizeAttributes(attributes, canonical)) {
      CountDrop(stats_);
      return false;
    }
    return WithActiveState(state_, stats_, [&] {
      std::lock_guard lock(values_mutex_);
      auto existing = values_.find(canonical);
      if (existing != values_.end()) {
        existing->second = static_cast<T>(value);
        return true;
      }
      if (values_.size() >= series_limit_) {
        stats_->rejected_gauge_series.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      values_.emplace(std::move(canonical), static_cast<T>(value));
      return true;
    });
  }

  static void Observe(api_metrics::ObserverResult result,
                      void *state) noexcept {
    auto *self = static_cast<GaugeInstrument *>(state);
    try {
      std::vector<std::pair<Attributes, T>> snapshot;
      {
        std::lock_guard lock(self->values_mutex_);
        snapshot.reserve(self->values_.size());
        for (const auto &entry : self->values_)
          snapshot.push_back(entry);
      }
      auto observer = opentelemetry::nostd::get<
          opentelemetry::nostd::shared_ptr<api_metrics::ObserverResultT<T>>>(
          result);
      for (const auto &entry : snapshot) {
        auto attributes = ToOtelAttributes(entry.first);
        observer->Observe(entry.second, attributes);
      }
    } catch (...) {
      CountDrop(self->stats_);
    }
  }

  std::weak_ptr<RuntimeState> state_;
  std::shared_ptr<AtomicStats> stats_;
  opentelemetry::nostd::shared_ptr<api_metrics::ObservableInstrument>
      instrument_;
  std::size_t series_limit_;
  std::mutex values_mutex_;
  std::map<Attributes, T, AttributesLess> values_;
};

Status ValidateInstrument(const InstrumentDescriptor &descriptor) {
  if (descriptor.name.empty()) {
    return {StatusCode::kInvalidArgument, "instrument name must not be empty"};
  }
  if (descriptor.kind == detail::InstrumentKind::kHistogram &&
      !descriptor.boundaries.empty()) {
    double previous = -1;
    for (double boundary : descriptor.boundaries) {
      if (!IsFinite(boundary) || boundary < 0 || boundary <= previous) {
        return {StatusCode::kInvalidArgument,
                "Histogram boundaries must be finite, non-negative, and "
                "strictly increasing"};
      }
      previous = boundary;
    }
  }
  if (descriptor.kind == detail::InstrumentKind::kGauge &&
      descriptor.gauge_series_limit.value_or(0) == 0) {
    return {StatusCode::kInvalidArgument,
            "Gauge series limit must be positive"};
  }
  return Status::Ok();
}

void AddHistogramView(RuntimeState &state,
                      const InstrumentDescriptor &descriptor) {
  auto aggregation =
      std::make_shared<sdk_metrics::HistogramAggregationConfig>();
  aggregation->boundaries_ = descriptor.boundaries;
  state.provider->AddView(
      sdk_metrics::InstrumentSelectorFactory::Create(
          sdk_metrics::InstrumentType::kHistogram, descriptor.name,
          descriptor.unit),
      sdk_metrics::MeterSelectorFactory::Create(state.config.scope_name,
                                                state.config.scope_version, ""),
      sdk_metrics::ViewFactory::Create(descriptor.name, descriptor.description,
                                       sdk_metrics::AggregationType::kHistogram,
                                       aggregation));
}

std::shared_ptr<detail::Instrument>
MakeInstrument(const std::shared_ptr<RuntimeState> &state,
               const InstrumentDescriptor &descriptor) {
  if (state->config.disabled) {
    return std::make_shared<NoopInstrument>(state, state->stats);
  }

  const auto &name = descriptor.name;
  const auto &description = descriptor.description;
  const auto &unit = descriptor.unit;
  if (descriptor.kind == detail::InstrumentKind::kCounter) {
    if (descriptor.number_kind == detail::NumberKind::kUInt64) {
      return std::make_shared<CounterInstrument<std::uint64_t>>(
          state, state->stats,
          state->meter->CreateUInt64Counter(name, description, unit));
    }
    return std::make_shared<CounterInstrument<double>>(
        state, state->stats,
        state->meter->CreateDoubleCounter(name, description, unit));
  }
  if (descriptor.kind == detail::InstrumentKind::kUpDownCounter) {
    if (descriptor.number_kind == detail::NumberKind::kInt64) {
      return std::make_shared<UpDownCounterInstrument<std::int64_t>>(
          state, state->stats,
          state->meter->CreateInt64UpDownCounter(name, description, unit));
    }
    return std::make_shared<UpDownCounterInstrument<double>>(
        state, state->stats,
        state->meter->CreateDoubleUpDownCounter(name, description, unit));
  }
  if (descriptor.kind == detail::InstrumentKind::kHistogram) {
    if (!descriptor.boundaries.empty())
      AddHistogramView(*state, descriptor);
    if (descriptor.number_kind == detail::NumberKind::kUInt64) {
      return std::make_shared<HistogramInstrument<std::uint64_t>>(
          state, state->stats,
          state->meter->CreateUInt64Histogram(name, description, unit));
    }
    return std::make_shared<HistogramInstrument<double>>(
        state, state->stats,
        state->meter->CreateDoubleHistogram(name, description, unit));
  }
  const auto limit = descriptor.gauge_series_limit.value();
  if (descriptor.number_kind == detail::NumberKind::kInt64) {
    return std::make_shared<GaugeInstrument<std::int64_t>>(
        state, state->stats,
        state->meter->CreateInt64ObservableGauge(name, description, unit),
        limit);
  }
  return std::make_shared<GaugeInstrument<double>>(
      state, state->stats,
      state->meter->CreateDoubleObservableGauge(name, description, unit),
      limit);
}

RuntimeStats SnapshotStats(const std::shared_ptr<AtomicStats> &stats,
                           bool initialized, bool disabled) {
  RuntimeStats snapshot;
  snapshot.initialized = initialized;
  snapshot.disabled = disabled;
  if (!stats)
    return snapshot;
  snapshot.successful_exports =
      stats->successful_exports.load(std::memory_order_relaxed);
  snapshot.failed_exports =
      stats->failed_exports.load(std::memory_order_relaxed);
  snapshot.dropped_measurements =
      stats->dropped_measurements.load(std::memory_order_relaxed);
  snapshot.rejected_gauge_series =
      stats->rejected_gauge_series.load(std::memory_order_relaxed);
  return snapshot;
}

std::chrono::microseconds
Remaining(std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline)
    return std::chrono::microseconds::zero();
  return std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
}

Status InitializeRuntime(
    const Config &config,
    std::unique_ptr<sdk_metrics::PushMetricExporter> supplied_exporter) {
  auto resolved = ResolveConfig(config);
  if (!resolved)
    return resolved.status();

  std::lock_guard runtime_lock(g_runtime_mutex);
  if (g_runtime && g_runtime->active) {
    if (g_runtime->config.signature == resolved.value().signature)
      return Status::Ok();
    return {StatusCode::kAlreadyInitialized,
            "metrics runtime is already initialized with different effective "
            "configuration"};
  }

  auto state = std::make_shared<RuntimeState>();
  state->stats = std::make_shared<AtomicStats>();
  state->config = std::move(resolved).value();

  if (state->config.disabled) {
    g_last_stats = state->stats;
    g_runtime = std::move(state);
    return Status::Ok();
  }

  auto exporter = std::move(supplied_exporter);
  if (!exporter) {
    if (state->config.protocol == Protocol::kHttpProtobuf) {
      exporter = otlp::OtlpHttpMetricExporterFactory::Create(
          state->config.http_options);
    } else {
      exporter = otlp::OtlpGrpcMetricExporterFactory::Create(
          state->config.grpc_options);
    }
  }
  if (!exporter)
    return {StatusCode::kInternal, "failed to create metrics exporter"};

  auto stats_exporter =
      std::make_unique<StatsExporter>(std::move(exporter), state->stats);
  auto reader = sdk_metrics::PeriodicExportingMetricReaderFactory::Create(
      std::move(stats_exporter), state->config.reader_options);
  auto context = sdk_metrics::MeterContextFactory::Create(
      std::make_unique<sdk_metrics::ViewRegistry>(), state->config.resource);
  context->AddMetricReader(std::move(reader));
  state->provider =
      sdk_metrics::MeterProviderFactory::Create(std::move(context));
  if (!state->provider) {
    return {StatusCode::kInternal,
            "failed to create OpenTelemetry MeterProvider"};
  }
  state->meter = state->provider->GetMeter(state->config.scope_name,
                                           state->config.scope_version);
  if (!state->meter)
    return {StatusCode::kInternal, "failed to create OpenTelemetry Meter"};

  g_last_stats = state->stats;
  g_runtime = std::move(state);
  return Status::Ok();
}

} // namespace

namespace detail {

bool Instrument::RecordUInt64(std::uint64_t, const Attributes &) noexcept {
  return false;
}
bool Instrument::RecordInt64(std::int64_t, const Attributes &) noexcept {
  return false;
}
bool Instrument::RecordDouble(double, const Attributes &) noexcept {
  return false;
}
bool Instrument::Remove(const Attributes &) noexcept { return false; }
std::size_t Instrument::Clear() noexcept { return 0; }

const Attributes &EmptyAttributes() noexcept {
  static const Attributes empty;
  return empty;
}

InstrumentCreation
CreateInstrument(InstrumentKind kind, NumberKind number_kind,
                 const InstrumentOptions &options,
                 const std::vector<double> &boundaries,
                 std::optional<std::size_t> gauge_series_limit) {
  try {
    std::shared_ptr<RuntimeState> state;
    {
      std::lock_guard runtime_lock(g_runtime_mutex);
      state = g_runtime;
    }
    if (!state) {
      return {
          {StatusCode::kNotInitialized, "metrics runtime is not initialized"},
          nullptr};
    }
    std::shared_lock activity_lock(state->activity_mutex);
    if (!state->active) {
      return {{StatusCode::kNotInitialized, "metrics runtime is shutting down"},
              nullptr};
    }

    InstrumentDescriptor descriptor{
        kind,         number_kind, options.name,      options.description,
        options.unit, boundaries,  gauge_series_limit};
    if (kind == InstrumentKind::kGauge && !descriptor.gauge_series_limit) {
      descriptor.gauge_series_limit = state->config.default_gauge_series_limit;
    }
    const auto validation = ValidateInstrument(descriptor);
    if (!validation)
      return {validation, nullptr};

    std::lock_guard registry_lock(state->registry_mutex);
    const auto key = LowerAscii(descriptor.name);
    const auto existing = state->instruments.find(key);
    if (existing != state->instruments.end()) {
      if (!existing->second.first.SameDefinition(descriptor)) {
        return {{StatusCode::kInstrumentConflict,
                 "instrument name is already registered with a different "
                 "definition: " +
                     descriptor.name},
                nullptr};
      }
      return {Status::Ok(), existing->second.second};
    }

    auto instrument = MakeInstrument(state, descriptor);
    if (!instrument) {
      return {
          {StatusCode::kInternal, "OpenTelemetry failed to create instrument"},
          nullptr};
    }
    state->instruments.emplace(
        key, std::make_pair(std::move(descriptor), instrument));
    return {Status::Ok(), std::move(instrument)};
  } catch (const std::exception &error) {
    return {{StatusCode::kInternal, error.what()}, nullptr};
  } catch (...) {
    return {{StatusCode::kInternal, "unknown error while creating instrument"},
            nullptr};
  }
}

} // namespace detail

Status Initialize(const Config &config) {
  try {
    return InitializeRuntime(config, nullptr);
  } catch (const std::exception &error) {
    return {StatusCode::kInternal, error.what()};
  } catch (...) {
    return {StatusCode::kInternal,
            "unknown error while initializing metrics runtime"};
  }
}

namespace testing {

Status InitializeWithExporter(
    const Config &config,
    std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> exporter) {
  try {
    if (!exporter) {
      return {StatusCode::kInvalidArgument, "test exporter must not be null"};
    }
    return InitializeRuntime(config, std::move(exporter));
  } catch (const std::exception &error) {
    return {StatusCode::kInternal, error.what()};
  } catch (...) {
    return {StatusCode::kInternal,
            "unknown error while initializing test metrics runtime"};
  }
}

} // namespace testing

Status ForceFlush(std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0) {
    return {StatusCode::kInvalidArgument, "flush timeout must be positive"};
  }
  std::shared_ptr<RuntimeState> state;
  {
    std::lock_guard runtime_lock(g_runtime_mutex);
    state = g_runtime;
  }
  if (!state)
    return {StatusCode::kNotInitialized, "metrics runtime is not initialized"};
  std::shared_lock activity_lock(state->activity_mutex);
  if (!state->active)
    return {StatusCode::kNotInitialized, "metrics runtime is shutting down"};
  if (state->config.disabled)
    return Status::Ok();
  if (!state->provider->ForceFlush(timeout)) {
    return {StatusCode::kTimeout, "metrics force flush failed or timed out"};
  }
  return Status::Ok();
}

Status Shutdown(std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0) {
    return {StatusCode::kInvalidArgument, "shutdown timeout must be positive"};
  }
  std::lock_guard runtime_lock(g_runtime_mutex);
  if (!g_runtime)
    return Status::Ok();

  auto state = g_runtime;
  std::unique_lock activity_lock(state->activity_mutex);
  state->active = false;
  if (state->config.disabled) {
    state->instruments.clear();
    g_runtime.reset();
    return Status::Ok();
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  bool flushed = state->provider->ForceFlush(Remaining(deadline));
  state->instruments.clear();
  state->meter = nullptr;
  // MeterProvider's destructor is the SDK's one-shot shutdown path. Calling
  // MeterProvider::Shutdown first makes its destructor emit a spurious
  // "Shutdown can be invoked only once" warning in opentelemetry-cpp 1.26.
  state->provider.reset();
  const bool shut_down = std::chrono::steady_clock::now() <= deadline;
  g_runtime.reset();
  if (!flushed || !shut_down) {
    return {StatusCode::kTimeout,
            "metrics flush or shutdown failed within the timeout"};
  }
  return Status::Ok();
}

bool IsInitialized() noexcept {
  try {
    std::lock_guard lock(g_runtime_mutex);
    return g_runtime && g_runtime->active;
  } catch (...) {
    return false;
  }
}

RuntimeStats GetRuntimeStats() noexcept {
  try {
    std::lock_guard lock(g_runtime_mutex);
    return SnapshotStats(g_runtime ? g_runtime->stats : g_last_stats,
                         g_runtime && g_runtime->active,
                         g_runtime && g_runtime->config.disabled);
  } catch (...) {
    return {};
  }
}

} // namespace otel_metrics
