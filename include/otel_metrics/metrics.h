#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace otel_metrics {

enum class StatusCode {
  kOk = 0,
  kInvalidArgument,
  kAlreadyInitialized,
  kNotInitialized,
  kInstrumentConflict,
  kTimeout,
  kInternal,
};

class Status {
public:
  Status() = default;
  Status(StatusCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  static Status Ok() { return {}; }

  bool ok() const noexcept { return code_ == StatusCode::kOk; }
  explicit operator bool() const noexcept { return ok(); }
  StatusCode code() const noexcept { return code_; }
  const std::string &message() const noexcept { return message_; }

private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

template <typename T> class Result {
public:
  static Result Success(T value) {
    return Result(Status::Ok(), std::move(value));
  }

  static Result Failure(Status status) { return Result(std::move(status)); }

  bool ok() const noexcept { return status_.ok(); }
  explicit operator bool() const noexcept { return ok(); }
  const Status &status() const noexcept { return status_; }
  T &value() & { return value_.value(); }
  const T &value() const & { return value_.value(); }
  T &&value() && { return std::move(value_).value(); }

private:
  explicit Result(Status status) : status_(std::move(status)) {}
  Result(Status status, T value)
      : status_(std::move(status)), value_(std::move(value)) {}

  Status status_;
  std::optional<T> value_;
};

using AttributeValue = std::variant<std::string, bool, std::int64_t, double>;

struct Attribute {
  std::string key;
  AttributeValue value;

  Attribute(std::string attribute_key, std::string attribute_value)
      : key(std::move(attribute_key)), value(std::move(attribute_value)) {}
  Attribute(std::string attribute_key, std::string_view attribute_value)
      : key(std::move(attribute_key)), value(std::string(attribute_value)) {}
  Attribute(std::string attribute_key, const char *attribute_value)
      : key(std::move(attribute_key)),
        value(std::string(attribute_value == nullptr ? "" : attribute_value)) {}
  Attribute(std::string attribute_key, bool attribute_value)
      : key(std::move(attribute_key)), value(attribute_value) {}

  template <typename T,
            std::enable_if_t<std::is_integral_v<std::decay_t<T>> &&
                                 !std::is_same_v<std::decay_t<T>, bool>,
                             int> = 0>
  Attribute(std::string attribute_key, T attribute_value)
      : key(std::move(attribute_key)),
        value(static_cast<std::int64_t>(attribute_value)) {}

  template <typename T, std::enable_if_t<
                            std::is_floating_point_v<std::decay_t<T>>, int> = 0>
  Attribute(std::string attribute_key, T attribute_value)
      : key(std::move(attribute_key)),
        value(static_cast<double>(attribute_value)) {}
};

using Attributes = std::vector<Attribute>;

enum class Protocol {
  kHttpProtobuf,
  kGrpc,
};

struct Config {
  std::string service_name;
  Attributes resource_attributes;
  std::string instrumentation_scope_name = "otel_metrics";
  std::string instrumentation_scope_version = "0.1.0";
  std::optional<Protocol> protocol;
  std::optional<std::string> endpoint;
  std::optional<std::chrono::milliseconds> export_interval;
  std::optional<std::chrono::milliseconds> export_timeout;
  std::optional<std::chrono::milliseconds> request_timeout;
  std::size_t default_gauge_series_limit = 1024;
};

struct InstrumentOptions {
  std::string name;
  std::string description;
  std::string unit;
};

struct HistogramOptions : InstrumentOptions {
  std::vector<double> boundaries;
};

struct GaugeOptions : InstrumentOptions {
  std::optional<std::size_t> series_limit;
};

struct RuntimeStats {
  bool initialized = false;
  bool disabled = false;
  std::uint64_t successful_exports = 0;
  std::uint64_t failed_exports = 0;
  std::uint64_t dropped_measurements = 0;
  std::uint64_t rejected_gauge_series = 0;
};

Status Initialize(const Config &config = {});
Status ForceFlush(std::chrono::milliseconds timeout);
Status Shutdown(std::chrono::milliseconds timeout);
bool IsInitialized() noexcept;
RuntimeStats GetRuntimeStats() noexcept;

namespace detail {

enum class InstrumentKind { kCounter, kUpDownCounter, kHistogram, kGauge };
enum class NumberKind { kUInt64, kInt64, kDouble };

class Instrument {
public:
  virtual ~Instrument() = default;
  virtual bool RecordUInt64(std::uint64_t, const Attributes &) noexcept;
  virtual bool RecordInt64(std::int64_t, const Attributes &) noexcept;
  virtual bool RecordDouble(double, const Attributes &) noexcept;
  virtual bool Remove(const Attributes &) noexcept;
  virtual std::size_t Clear() noexcept;
};

struct InstrumentCreation {
  Status status;
  std::shared_ptr<Instrument> instrument;
};

InstrumentCreation
CreateInstrument(InstrumentKind kind, NumberKind number_kind,
                 const InstrumentOptions &options,
                 const std::vector<double> &boundaries,
                 std::optional<std::size_t> gauge_series_limit);
const Attributes &EmptyAttributes() noexcept;

template <typename> inline constexpr bool kAlwaysFalse = false;

} // namespace detail

template <typename T> class Counter {
  static_assert(std::is_same_v<T, std::uint64_t> || std::is_same_v<T, double>,
                "Counter supports only uint64_t and double");

public:
  Counter() = default;
  explicit operator bool() const noexcept {
    return static_cast<bool>(instrument_);
  }

  bool Add(T value) const noexcept {
    return Add(value, detail::EmptyAttributes());
  }
  bool Add(T value, const Attributes &attributes) const noexcept {
    if (!instrument_)
      return false;
    if constexpr (std::is_same_v<T, std::uint64_t>) {
      return instrument_->RecordUInt64(value, attributes);
    } else {
      return instrument_->RecordDouble(value, attributes);
    }
  }

private:
  explicit Counter(std::shared_ptr<detail::Instrument> instrument)
      : instrument_(std::move(instrument)) {}
  std::shared_ptr<detail::Instrument> instrument_;

  template <typename U>
  friend Result<Counter<U>> CreateCounter(const InstrumentOptions &);
};

template <typename T> class UpDownCounter {
  static_assert(std::is_same_v<T, std::int64_t> || std::is_same_v<T, double>,
                "UpDownCounter supports only int64_t and double");

public:
  UpDownCounter() = default;
  explicit operator bool() const noexcept {
    return static_cast<bool>(instrument_);
  }

  bool Add(T value) const noexcept {
    return Add(value, detail::EmptyAttributes());
  }
  bool Add(T value, const Attributes &attributes) const noexcept {
    if (!instrument_)
      return false;
    if constexpr (std::is_same_v<T, std::int64_t>) {
      return instrument_->RecordInt64(value, attributes);
    } else {
      return instrument_->RecordDouble(value, attributes);
    }
  }

private:
  explicit UpDownCounter(std::shared_ptr<detail::Instrument> instrument)
      : instrument_(std::move(instrument)) {}
  std::shared_ptr<detail::Instrument> instrument_;

  template <typename U>
  friend Result<UpDownCounter<U>>
  CreateUpDownCounter(const InstrumentOptions &);
};

template <typename T> class Histogram {
  static_assert(std::is_same_v<T, std::uint64_t> || std::is_same_v<T, double>,
                "Histogram supports only uint64_t and double");

public:
  Histogram() = default;
  explicit operator bool() const noexcept {
    return static_cast<bool>(instrument_);
  }

  bool Record(T value) const noexcept {
    return Record(value, detail::EmptyAttributes());
  }
  bool Record(T value, const Attributes &attributes) const noexcept {
    if (!instrument_)
      return false;
    if constexpr (std::is_same_v<T, std::uint64_t>) {
      return instrument_->RecordUInt64(value, attributes);
    } else {
      return instrument_->RecordDouble(value, attributes);
    }
  }

private:
  explicit Histogram(std::shared_ptr<detail::Instrument> instrument)
      : instrument_(std::move(instrument)) {}
  std::shared_ptr<detail::Instrument> instrument_;

  template <typename U>
  friend Result<Histogram<U>> CreateHistogram(const HistogramOptions &);
};

template <typename T> class Gauge {
  static_assert(std::is_same_v<T, std::int64_t> || std::is_same_v<T, double>,
                "Gauge supports only int64_t and double");

public:
  Gauge() = default;
  explicit operator bool() const noexcept {
    return static_cast<bool>(instrument_);
  }

  bool Set(T value) const noexcept {
    return Set(value, detail::EmptyAttributes());
  }
  bool Set(T value, const Attributes &attributes) const noexcept {
    if (!instrument_)
      return false;
    if constexpr (std::is_same_v<T, std::int64_t>) {
      return instrument_->RecordInt64(value, attributes);
    } else {
      return instrument_->RecordDouble(value, attributes);
    }
  }
  bool Remove(const Attributes &attributes = {}) const noexcept {
    return instrument_ && instrument_->Remove(attributes);
  }
  std::size_t Clear() const noexcept {
    return instrument_ ? instrument_->Clear() : 0;
  }

private:
  explicit Gauge(std::shared_ptr<detail::Instrument> instrument)
      : instrument_(std::move(instrument)) {}
  std::shared_ptr<detail::Instrument> instrument_;

  template <typename U>
  friend Result<Gauge<U>> CreateGauge(const GaugeOptions &);
};

template <typename T>
Result<Counter<T>> CreateCounter(const InstrumentOptions &options) {
  const auto number_kind = std::is_same_v<T, std::uint64_t>
                               ? detail::NumberKind::kUInt64
                               : detail::NumberKind::kDouble;
  auto created = detail::CreateInstrument(
      detail::InstrumentKind::kCounter, number_kind, options, {}, std::nullopt);
  if (!created.status)
    return Result<Counter<T>>::Failure(std::move(created.status));
  return Result<Counter<T>>::Success(Counter<T>(std::move(created.instrument)));
}

template <typename T>
Result<UpDownCounter<T>> CreateUpDownCounter(const InstrumentOptions &options) {
  const auto number_kind = std::is_same_v<T, std::int64_t>
                               ? detail::NumberKind::kInt64
                               : detail::NumberKind::kDouble;
  auto created =
      detail::CreateInstrument(detail::InstrumentKind::kUpDownCounter,
                               number_kind, options, {}, std::nullopt);
  if (!created.status)
    return Result<UpDownCounter<T>>::Failure(std::move(created.status));
  return Result<UpDownCounter<T>>::Success(
      UpDownCounter<T>(std::move(created.instrument)));
}

template <typename T>
Result<Histogram<T>> CreateHistogram(const HistogramOptions &options) {
  const auto number_kind = std::is_same_v<T, std::uint64_t>
                               ? detail::NumberKind::kUInt64
                               : detail::NumberKind::kDouble;
  auto created =
      detail::CreateInstrument(detail::InstrumentKind::kHistogram, number_kind,
                               options, options.boundaries, std::nullopt);
  if (!created.status)
    return Result<Histogram<T>>::Failure(std::move(created.status));
  return Result<Histogram<T>>::Success(
      Histogram<T>(std::move(created.instrument)));
}

template <typename T>
Result<Gauge<T>> CreateGauge(const GaugeOptions &options) {
  const auto number_kind = std::is_same_v<T, std::int64_t>
                               ? detail::NumberKind::kInt64
                               : detail::NumberKind::kDouble;
  auto created =
      detail::CreateInstrument(detail::InstrumentKind::kGauge, number_kind,
                               options, {}, options.series_limit);
  if (!created.status)
    return Result<Gauge<T>>::Failure(std::move(created.status));
  return Result<Gauge<T>>::Success(Gauge<T>(std::move(created.instrument)));
}

} // namespace otel_metrics
