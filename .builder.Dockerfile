# Build environment for compiling UE4SS Linux, matching the fork's CI
# (ubuntu-24.04 + cmake/ninja + Rust for patternsleuth + X11/GL dev libs).
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake ninja-build pkg-config \
        libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
        libgl-dev libegl-dev libgles-dev \
        gcc g++ binutils curl git ca-certificates \
    && rm -rf /var/lib/apt/lists/*
# Rust toolchain (patternsleuth is Rust, built via Corrosion)
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | \
        sh -s -- -y --default-toolchain stable --profile minimal -c rustfmt
ENV PATH=/root/.cargo/bin:$PATH
WORKDIR /src
