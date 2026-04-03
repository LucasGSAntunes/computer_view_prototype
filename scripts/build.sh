#!/bin/bash
# Build script for Vision Platform
set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"

echo "=== Vision Platform Build ==="
echo "Project: ${PROJECT_DIR}"
echo "Build:   ${BUILD_DIR}"
echo ""

# Options
ONNX=${VP_WITH_ONNX:-ON}
POSTGRES=${VP_WITH_POSTGRES:-ON}
REDIS=${VP_WITH_REDIS:-ON}
BUILD_TYPE=${BUILD_TYPE:-Release}

# Clean build option
if [ "$1" = "clean" ]; then
    echo "Cleaning build directory..."
    rm -rf "${BUILD_DIR}"
fi

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake "${PROJECT_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DVP_WITH_ONNX="${ONNX}" \
    -DVP_WITH_POSTGRES="${POSTGRES}" \
    -DVP_WITH_REDIS="${REDIS}" \
    ${ONNXRUNTIME_ROOT:+-DONNXRUNTIME_ROOT="${ONNXRUNTIME_ROOT}"}

cmake --build . -j "$(nproc)"

echo ""
echo "=== Build complete ==="
echo "Binaries:"
ls -la vp_worker vp_ingestion vp_scheduler vp_export 2>/dev/null || true
echo ""
echo "Usage:"
echo "  ./build/vp_worker -i video.mp4 -m model.onnx -l labels.txt -t"
echo "  ./build/vp_ingestion -m model.onnx -l labels.txt"
echo "  ./build/vp_scheduler -m model.onnx -l labels.txt"
echo "  ./build/vp_export -j <job_id>"
