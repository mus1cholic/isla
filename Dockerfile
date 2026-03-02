# Start with the official Github ARC image (Ubuntu 22.04)
FROM ghcr.io/actions/actions-runner:latest

LABEL org.opencontainers.image.source="https://github.com/mus1cholic/isla"

USER root

# Pin the Bazel version used by Bazelisk
ENV USE_BAZEL_VERSION=9.0.0
ENV LLVM_MINGW_VERSION=20251216

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
        wget \
        curl \
        git \
        g++ \
        ca-certificates \
        apt-transport-https \
        software-properties-common \
        lsb-release \
        gnupg \
        jq \
        clang \
        clang-tidy \
        clang-format \
        cmake \
        ninja-build \
        pkg-config \
        xz-utils \
        libasound2-dev \
        libpulse-dev \
        libx11-dev \
        libxext-dev \
        libxrandr-dev \
        libxcursor-dev \
        libxfixes-dev \
        libxi-dev \
        libxss-dev \
        libxtst-dev \
        libwayland-dev \
        libxkbcommon-dev \
        libdrm-dev \
        libgbm-dev \
        libxcb1-dev; \
    \
    # Install PowerShell
    wget -q https://packages.microsoft.com/config/ubuntu/22.04/packages-microsoft-prod.deb; \
    dpkg -i packages-microsoft-prod.deb; \
    apt-get update; \
    apt-get install -y --no-install-recommends powershell; \
    rm -f packages-microsoft-prod.deb; \
    \
    # Install Bazelisk as `bazel` (x86_64)
    curl -fsSL -o /usr/local/bin/bazel \
      https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64; \
    chmod +x /usr/local/bin/bazel; \
    \
    # Install BuildBuddy CLI as `bb` (latest, but without the sudo-requiring installer)
    BB_URL="$(curl -fsSL https://api.github.com/repos/buildbuddy-io/bazel/releases/latest \
      | jq -r '.assets[].browser_download_url' \
      | grep -E 'linux-x86_64$' \
      | head -n 1)"; \
    test -n "$BB_URL"; \
    curl -fsSL -o /usr/local/bin/bb "$BB_URL"; \
    chmod +x /usr/local/bin/bb; \
    \
    # Install llvm-mingw toolchain for Linux-hosted Windows cross-builds.
    LLVM_MINGW_ARCHIVE="llvm-mingw-${LLVM_MINGW_VERSION}-ucrt-ubuntu-22.04-x86_64.tar.xz"; \
    LLVM_MINGW_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_VERSION}/${LLVM_MINGW_ARCHIVE}"; \
    mkdir -p /opt/llvm-mingw; \
    curl -fsSL -o "/tmp/${LLVM_MINGW_ARCHIVE}" "${LLVM_MINGW_URL}"; \
    tar -xf "/tmp/${LLVM_MINGW_ARCHIVE}" -C /opt/llvm-mingw; \
    rm -f "/tmp/${LLVM_MINGW_ARCHIVE}"; \
    ln -sfn "/opt/llvm-mingw/llvm-mingw-${LLVM_MINGW_VERSION}-ucrt-ubuntu-22.04-x86_64" /opt/llvm-mingw/current; \
    test -f /opt/llvm-mingw/current/x86_64-w64-mingw32/include/pthread.h; \
    /opt/llvm-mingw/current/bin/x86_64-w64-mingw32-clang++ --version; \
    \
    # Optional sanity checks (fail fast during image build)
    bazel --version; \
    clang-tidy --version; \
    clang-format --version; \
    g++ --version; \
    bb --version; \
    \
    apt-get clean; \
    rm -rf /var/lib/apt/lists/*

USER runner

