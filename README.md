# otel_metrics

`otel_metrics` 是一个仅面向 Metrics 的 C++17 OpenTelemetry 封装库。公共头文件不暴露
OpenTelemetry 类型，库内部负责 OTLP exporter、MeterProvider、Resource、Gauge 回调、
并发与 flush/shutdown 生命周期。

## 构建与测试

需要 CMake 3.24+、Conan 2 和支持 C++17 的编译器：

```bash
./scripts/build.sh
```

构建会安装 `opentelemetry-cpp/1.26.0` 依赖，并同时启用 OTLP/HTTP protobuf 和
OTLP/gRPC。

## 作为 CMake 子项目使用

消费方首先通过 Conan 提供 `opentelemetry-cpp/1.26.0`，然后在 CMake 中引入本目录：

```cmake
find_package(opentelemetry-cpp CONFIG REQUIRED)
set(OTEL_METRICS_BUILD_TESTING OFF)
add_subdirectory(path/to/otel_metrics otel-metrics-build)

target_link_libraries(my_target PRIVATE otel_metrics::otel_metrics)
```

业务代码只需要：

```cpp
#include "otel_metrics/metrics.h"

otel_metrics::Config config;
config.service_name = "my-service";
auto status = otel_metrics::Initialize(config);

auto result = otel_metrics::CreateCounter<std::uint64_t>(
    {"http.server.requests", "Handled requests", "{request}"});
if (status && result) {
  auto counter = std::move(result).value();
  counter.Add(1, {{"http.request.method", "GET"}});
}

otel_metrics::Shutdown(std::chrono::seconds(10));
```

支持 `Counter<uint64_t|double>`、`UpDownCounter<int64_t|double>`、
`Histogram<uint64_t|double>` 和 `Gauge<int64_t|double>`。完整接口见
`include/otel_metrics/metrics.h`。
