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

## 运行时开关

程序编译后可以在启动时通过标准 OpenTelemetry 环境变量控制是否采集和发送
Metrics：

```bash
# 关闭 Metrics
OTEL_SDK_DISABLED=true ./your_application

# 开启 Metrics（未设置时也默认开启）
OTEL_SDK_DISABLED=false ./your_application
```

环境变量值 `true` 和 `false` 不区分大小写。关闭时 `Initialize()`、指标创建、
`Add()`、`Record()`、`Set()` 和 `ForceFlush()` 仍然成功，但它们是 no-op：不会创建
OTLP exporter，不聚合或发送数据，也不会保留 Gauge series。业务代码不需要为开关
增加条件分支。可以通过 `GetRuntimeStats().disabled` 查看本次初始化是否处于禁用状态。

开关在 `Initialize()` 时读取；修改运行中进程的环境变量不会即时切换。如需切换，
应重启进程，或者先 `Shutdown()`、修改环境变量、重新 `Initialize()` 并重新创建所有
指标句柄。
