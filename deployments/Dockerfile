FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential cmake pkg-config \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
    libpq-dev libhiredis-dev uuid-dev \
    && rm -rf /var/lib/apt/lists/*

# ONNX Runtime
ARG ONNXRUNTIME_VERSION=1.17.0
RUN apt-get update && apt-get install -y wget && \
    wget -q https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}.tgz && \
    tar xzf onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}.tgz && \
    cp -r onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}/include/* /usr/local/include/ && \
    cp -r onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}/lib/* /usr/local/lib/ && \
    ldconfig && \
    rm -rf onnxruntime-*

WORKDIR /app
COPY . .

RUN mkdir -p build && cd build && \
    cmake .. \
      -DCMAKE_BUILD_TYPE=Release \
      -DVP_WITH_ONNX=ON \
      -DVP_WITH_POSTGRES=ON \
      -DVP_WITH_REDIS=ON && \
    cmake --build . -j$(nproc)

# ── Runtime image ──
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
    libpq5 libhiredis0.14 libuuid1 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/lib/libonnxruntime* /usr/local/lib/
RUN ldconfig

WORKDIR /app
COPY --from=builder /app/build/vp_worker /app/build/vp_worker
COPY --from=builder /app/build/vp_ingestion /app/build/vp_ingestion
COPY --from=builder /app/build/vp_scheduler /app/build/vp_scheduler
COPY --from=builder /app/build/vp_export /app/build/vp_export

RUN mkdir -p /app/output /app/models

CMD ["/app/build/vp_worker"]
