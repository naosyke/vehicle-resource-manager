# Development and runtime image: Ubuntu + Eclipse Cyclone DDS (C and C++).
FROM ubuntu:24.04 AS base

ARG CYCLONEDDS_VERSION=0.10.5
ARG CYCLONEDDS_CXX_VERSION=0.10.5

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates \
        libyaml-cpp-dev libgtest-dev procps \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch ${CYCLONEDDS_VERSION} https://github.com/eclipse-cyclonedds/cyclonedds.git /tmp/cyclonedds \
    && cmake -S /tmp/cyclonedds -B /tmp/cyclonedds/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DENABLE_SSL=OFF \
    && cmake --build /tmp/cyclonedds/build --target install \
    && git clone --depth 1 --branch ${CYCLONEDDS_CXX_VERSION} https://github.com/eclipse-cyclonedds/cyclonedds-cxx.git /tmp/cyclonedds-cxx \
    && cmake -S /tmp/cyclonedds-cxx -B /tmp/cyclonedds-cxx/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF \
    && cmake --build /tmp/cyclonedds-cxx/build --target install \
    && ldconfig \
    && rm -rf /tmp/cyclonedds /tmp/cyclonedds-cxx

WORKDIR /workspace

FROM base AS build
COPY . /workspace
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build \
    && ctest --test-dir build --output-on-failure

CMD ["build/vrm_manager", "config/system.yaml"]
