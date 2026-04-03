cmake_minimum_required(VERSION 3.16)
project(vision_platform C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Build options ──
option(VP_WITH_ONNX      "Build ONNX Runtime backend"  ON)
option(VP_WITH_TENSORRT   "Build TensorRT backend"      OFF)
option(VP_WITH_OPENVINO   "Build OpenVINO backend"      OFF)
option(VP_WITH_POSTGRES   "Build PostgreSQL support"     ON)
option(VP_WITH_REDIS      "Build Redis queue support"    ON)

# ── Compiler flags ──
add_compile_options(-Wall -Wextra -O2)

# ── Find dependencies ──
find_package(PkgConfig REQUIRED)

# FFmpeg
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC  REQUIRED libavcodec)
pkg_check_modules(AVUTIL   REQUIRED libavutil)
pkg_check_modules(SWSCALE  REQUIRED libswscale)

# Threads
find_package(Threads REQUIRED)

# uuid
pkg_check_modules(UUID uuid)
if(NOT UUID_FOUND)
    # Try manual find
    find_library(UUID_LIB uuid)
    if(UUID_LIB)
        set(UUID_LIBRARIES ${UUID_LIB})
        set(UUID_FOUND TRUE)
    endif()
endif()

# Math
set(VP_MATH_LIB m)

# ── Core library ──
set(CORE_SOURCES
    core/common/logging.c
    core/common/memory_pool.c
    core/pipeline/video_decoder.c
    core/pipeline/frame_sampler.c
    core/pipeline/preprocessor.c
    core/pipeline/postprocessor.c
    core/pipeline/tracker.c
    core/pipeline/catalog_builder.c
    core/pipeline/metrics_collector.c
    core/pipeline/trajectory_builder.c
    core/pipeline/event_detector.c
    core/pipeline/pipeline.c
    core/infra/exporter.c
    # Always compiled — have stubs when lib not available
    core/pipeline/backend_onnx.c
    core/infra/database.c
    core/infra/queue_redis.c
    core/infra/audit.c
    core/infra/comparison.c
)

# Inference backends
if(VP_WITH_ONNX)
    add_definitions(-DVP_HAS_ONNX)
    if(DEFINED ENV{ONNXRUNTIME_ROOT})
        set(ONNXRUNTIME_ROOT $ENV{ONNXRUNTIME_ROOT})
    elseif(NOT DEFINED ONNXRUNTIME_ROOT)
        set(ONNXRUNTIME_ROOT "/usr/local")
    endif()
    set(ONNXRUNTIME_INCLUDE ${ONNXRUNTIME_ROOT}/include)
    set(ONNXRUNTIME_LIB_DIR ${ONNXRUNTIME_ROOT}/lib)
endif()

# PostgreSQL
if(VP_WITH_POSTGRES)
    pkg_check_modules(LIBPQ libpq)
    if(LIBPQ_FOUND)
        add_definitions(-DVP_HAS_POSTGRES)
    else()
        message(WARNING "libpq not found — building without PostgreSQL support")
        set(VP_WITH_POSTGRES OFF)
    endif()
endif()

# Redis (hiredis)
if(VP_WITH_REDIS)
    pkg_check_modules(HIREDIS hiredis)
    if(HIREDIS_FOUND)
        add_definitions(-DVP_HAS_REDIS)
    else()
        message(WARNING "hiredis not found — building without Redis support")
        set(VP_WITH_REDIS OFF)
    endif()
endif()

add_library(vp_core STATIC ${CORE_SOURCES})

target_include_directories(vp_core PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
)

target_link_libraries(vp_core PUBLIC
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    Threads::Threads
    ${VP_MATH_LIB}
)

if(UUID_FOUND)
    target_include_directories(vp_core PUBLIC ${UUID_INCLUDE_DIRS})
    target_link_libraries(vp_core PUBLIC ${UUID_LIBRARIES})
endif()

if(VP_WITH_ONNX)
    target_include_directories(vp_core PUBLIC ${ONNXRUNTIME_INCLUDE})
    target_link_directories(vp_core PUBLIC ${ONNXRUNTIME_LIB_DIR})
    target_link_libraries(vp_core PUBLIC onnxruntime)
endif()

if(VP_WITH_POSTGRES)
    target_include_directories(vp_core PUBLIC ${LIBPQ_INCLUDE_DIRS})
    target_link_libraries(vp_core PUBLIC ${LIBPQ_LIBRARIES})
endif()

if(VP_WITH_REDIS)
    target_include_directories(vp_core PUBLIC ${HIREDIS_INCLUDE_DIRS})
    target_link_libraries(vp_core PUBLIC ${HIREDIS_LIBRARIES})
endif()

# ── Executables ──

# Worker (main pipeline executor)
add_executable(vp_worker cmd/worker/main.c)
target_link_libraries(vp_worker PRIVATE vp_core)

# Ingestion service
add_executable(vp_ingestion cmd/ingestion_service/main.c)
target_link_libraries(vp_ingestion PRIVATE vp_core)

# Scheduler / dispatcher
add_executable(vp_scheduler cmd/scheduler/main.c)
target_link_libraries(vp_scheduler PRIVATE vp_core)

# Export service
add_executable(vp_export cmd/export_service/main.c)
target_link_libraries(vp_export PRIVATE vp_core)

# ── Install ──
install(TARGETS vp_worker vp_ingestion vp_scheduler vp_export
        RUNTIME DESTINATION bin)

message(STATUS "── Vision Platform build config ──")
message(STATUS "  ONNX Runtime : ${VP_WITH_ONNX}")
message(STATUS "  TensorRT     : ${VP_WITH_TENSORRT}")
message(STATUS "  OpenVINO     : ${VP_WITH_OPENVINO}")
message(STATUS "  PostgreSQL   : ${VP_WITH_POSTGRES}")
message(STATUS "  Redis        : ${VP_WITH_REDIS}")
message(STATUS "  UUID         : ${UUID_FOUND}")
