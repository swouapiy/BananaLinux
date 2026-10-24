# The host checks' Linux, for a Mac (or any machine that isn't Ubuntu): CI's checks job's packages on CI's Ubuntu,
# plus what the theme studio's tests use. Not for image builds (those want a real Linux box: an hour or more).
#
#   tools/dev.sh                  a shell in it, the repo at /work
#   tools/dev.sh make test        one command in it
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential pkg-config git ca-certificates \
        libfreetype-dev libdrm-dev libasound2-dev zlib1g-dev imagemagick fonts-urw-base35 \
        g++-aarch64-linux-gnu gcc-aarch64-linux-gnu \
        util-linux procps \
        python3 python3-jsonschema nodejs \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
