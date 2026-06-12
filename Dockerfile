# Trix container image (multi-stage).
#
# The binary is built with gcc-15 (it references GLIBCXX_3.4.34 from
# std::format / std::print) and linked -static-libstdc++ -static-libgcc so it
# carries its own C++ runtime; the slim runtime image then needs only
# libc/libm + libreadline + zlib + tzdata.  Verified: after -static-libstdc++,
# `ldd` shows no libstdc++ / libgcc_s and the binary stays PIE (the Release
# -fhardened link options compose with the static flags).
#
#   docker build -t trix .
#   docker run --rm -it trix                                  # REPL
#   docker run --rm -v "$PWD:/work" trix /work/script.trx     # run a script
#   echo '[1 2 3] { 2 mul } map ==' | docker run --rm -i trix --stdin

# ---------- builder ----------
FROM ubuntu:24.04 AS builder

# gcc-15 from the toolchain PPA (ubuntu:24.04 ships gcc-14); zlib + readline are
# REQUIRED by the default build (find_package(ZLIB REQUIRED); readline FATAL if
# absent).
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        software-properties-common ca-certificates && \
    add-apt-repository -y ppa:ubuntu-toolchain-r/test && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        gcc-15 g++-15 cmake ninja-build \
        libreadline-dev zlib1g-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=gcc-15 -DCMAKE_CXX_COMPILER=g++-15 \
        -DTRIX_SANITIZERS=OFF \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" && \
    cmake --build build --target trix && \
    strip build/trix && \
    build/trix --version

# ---------- runtime ----------
FROM ubuntu:24.04

LABEL org.opencontainers.image.title="Trix" \
      org.opencontainers.image.description="Embeddable, single-header C++23 concatenative language: full-VM snapshots, transactional rollback, OTP-style supervision, logic queries." \
      org.opencontainers.image.licenses="Apache-2.0" \
      org.opencontainers.image.source="https://github.com/mcguidarelli/trix"

# libreadline8 (pulls libtinfo6) for the REPL, zlib1g for deflate/inflate,
# tzdata so the chrono instant-*-local ops resolve a zone (current_zone() reads
# /etc/localtime -- UTC in the base image; mount /etc/localtime to override).
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libreadline8 zlib1g tzdata && \
    rm -rf /var/lib/apt/lists/* && \
    useradd -m -s /bin/bash trix

COPY --from=builder /src/build/trix /usr/local/bin/trix

USER trix
WORKDIR /home/trix
ENTRYPOINT ["trix"]
CMD []
