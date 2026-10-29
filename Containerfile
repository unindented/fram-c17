FROM docker.io/library/ubuntu:26.04@sha256:c098726dd9b9733f3e885f8de0ef375206a311b0a317eadc40d347b1115fb944

ARG LLVM_VERSION=23
ARG GCC_VERSION=14

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates \
      cmake \
      cppcheck \
      ffmpeg \
      "gcc-${GCC_VERSION}" \
      libc6-dev \
      make \
      nasm \
      ninja-build \
      wget \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /etc/apt/keyrings \
    && wget --quiet https://apt.llvm.org/llvm-snapshot.gpg.key \
      --output-document=/etc/apt/keyrings/apt.llvm.org.asc \
    && echo \
      "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.asc] https://apt.llvm.org/resolute/ llvm-toolchain-resolute-${LLVM_VERSION} main" \
      > /etc/apt/sources.list.d/apt.llvm.org.list \
    && apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      "clang-${LLVM_VERSION}" \
      "clang-format-${LLVM_VERSION}" \
      "clang-tidy-${LLVM_VERSION}" \
      "libclang-rt-${LLVM_VERSION}-dev" \
    && rm -rf /var/lib/apt/lists/*

# cwrap has no distribution package, so the image installs a pinned release and checks its digest.
ARG CWRAP_VERSION=0.3.1
RUN case "$(uname -m)" in \
      x86_64) target=x86_64-linux-musl sha256=abb3b27c528cd04d1712e54da502b153b348454a650109a5acec88f2c680fd47 ;; \
      aarch64) target=aarch64-linux-musl sha256=a63d4312d16471e4360b4853fddd0425fea93b2b37fde3de77c400eb29575408 ;; \
      *) echo "no cwrap release for $(uname -m)" >&2; exit 1 ;; \
    esac \
    && archive="cwrap-${CWRAP_VERSION}-${target}.tar.gz" \
    && wget --quiet "https://github.com/unindented/cwrap-c17/releases/download/v${CWRAP_VERSION}/${archive}" \
      --output-document="/tmp/${archive}" \
    && echo "${sha256}  /tmp/${archive}" | sha256sum --check --quiet \
    && tar -xzf "/tmp/${archive}" -C /usr/local --strip-components=1 --no-same-owner \
      "cwrap-${CWRAP_VERSION}-${target}/bin/cwrap" \
    && rm "/tmp/${archive}"

ENV CC=clang-23

COPY . /src
WORKDIR /src

ENTRYPOINT ["cmake", "--workflow", "--preset"]
CMD ["ci-debug"]
