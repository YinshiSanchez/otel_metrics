cmake_minimum_required(VERSION 3.24)
project(otel_metrics VERSION 0.1.0 LANGUAGES CXX)

find_package(opentelemetry-cpp CONFIG REQUIRED)

option(OTEL_METRICS_BUILD_TESTING "Build otel_metrics unit tests"
       ${PROJECT_IS_TOP_LEVEL})

add_library(otel_metrics STATIC src/metrics.cpp)
add_library(otel_metrics::otel_metrics ALIAS otel_metrics)

target_compile_features(otel_metrics PUBLIC cxx_std_17)
target_include_directories(
  otel_metrics
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(
  otel_metrics
  PRIVATE opentelemetry-cpp::exporter_otlp_http_metric
          opentelemetry-cpp::exporter_otlp_grpc_metrics
          opentelemetry-cpp::metrics)

# ConanCenter's static opentelemetry-cpp 1.26 metadata places proto_grpc at
# the tail of the link line, after the protobuf and gRPC archives it consumes.
# Repeating those dependencies after the final metrics archive resolves the
# static archive cycle without forcing all OTEL objects into every consumer.
set_property(
  TARGET opentelemetry-cpp::metrics
  APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                  opentelemetry-cpp::proto
                  gRPC::grpc++)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(otel_metrics PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(OTEL_METRICS_BUILD_TESTING)
  include(CTest)
  enable_testing()
  find_library(
    OTEL_IN_MEMORY_METRIC_LIBRARY
    NAMES opentelemetry_exporter_in_memory_metric
    REQUIRED)
  add_executable(otel_metrics_tests tests/metrics_test.cpp)
  target_compile_features(otel_metrics_tests PRIVATE cxx_std_17)
  target_include_directories(otel_metrics_tests PRIVATE src)
  target_link_libraries(
    otel_metrics_tests
    PRIVATE otel_metrics::otel_metrics
            opentelemetry-cpp::exporter_in_memory
            "${OTEL_IN_MEMORY_METRIC_LIBRARY}")
  add_test(NAME otel_metrics_tests COMMAND otel_metrics_tests)
endif()
