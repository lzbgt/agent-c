FROM ubuntu:24.04 AS build

ARG HTTP_PROXY
ARG HTTPS_PROXY
ARG NO_PROXY
ENV HTTP_PROXY=${HTTP_PROXY}
ENV HTTPS_PROXY=${HTTPS_PROXY}
ENV NO_PROXY=${NO_PROXY}

ENV DEBIAN_FRONTEND=noninteractive
RUN set -e; \
  APT_OPTS="-o Acquire::Retries=3 -o Acquire::http::Timeout=20 -o Acquire::https::Timeout=20"; \
  if [ -n "${HTTP_PROXY}" ] || [ -n "${HTTPS_PROXY}" ]; then \
    { \
      echo "Acquire::http::Proxy \"${HTTP_PROXY}\";"; \
      echo "Acquire::https::Proxy \"${HTTPS_PROXY}\";"; \
    } > /etc/apt/apt.conf.d/99proxy; \
  fi; \
  apt-get update ${APT_OPTS} && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    ca-certificates \
    git \
    curl \
    libcurl4-openssl-dev \
    libjsoncpp-dev \
    libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/* \
  || ( \
    echo "apt via proxy failed; retrying without proxy" >&2; \
    rm -f /etc/apt/apt.conf.d/99proxy; \
    HTTP_PROXY= HTTPS_PROXY= NO_PROXY= \
      apt-get update ${APT_OPTS} && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        pkg-config \
        ca-certificates \
        git \
        curl \
        libcurl4-openssl-dev \
        libjsoncpp-dev \
        libsqlite3-dev \
        && rm -rf /var/lib/apt/lists/* \
  )

WORKDIR /src
COPY . /src

RUN cmake -S /src -B /src/docker_build -G Ninja -DCMAKE_BUILD_TYPE=Release
RUN cmake --build /src/docker_build -j 4

FROM ubuntu:24.04
ARG HTTP_PROXY
ARG HTTPS_PROXY
ARG NO_PROXY
ENV HTTP_PROXY=${HTTP_PROXY}
ENV HTTPS_PROXY=${HTTPS_PROXY}
ENV NO_PROXY=${NO_PROXY}
ENV DEBIAN_FRONTEND=noninteractive
RUN set -e; \
  APT_OPTS="-o Acquire::Retries=3 -o Acquire::http::Timeout=20 -o Acquire::https::Timeout=20"; \
  if [ -n "${HTTP_PROXY}" ] || [ -n "${HTTPS_PROXY}" ]; then \
    { \
      echo "Acquire::http::Proxy \"${HTTP_PROXY}\";"; \
      echo "Acquire::https::Proxy \"${HTTPS_PROXY}\";"; \
    } > /etc/apt/apt.conf.d/99proxy; \
  fi; \
  apt-get update ${APT_OPTS} && apt-get install -y --no-install-recommends \
    ca-certificates \
    libcurl4 \
    libjsoncpp25 \
    libsqlite3-0 \
    && rm -rf /var/lib/apt/lists/* \
  || ( \
    echo "apt via proxy failed; retrying without proxy" >&2; \
    rm -f /etc/apt/apt.conf.d/99proxy; \
    HTTP_PROXY= HTTPS_PROXY= NO_PROXY= \
      apt-get update ${APT_OPTS} && apt-get install -y --no-install-recommends \
        ca-certificates \
        libcurl4 \
        libjsoncpp25 \
        libsqlite3-0 \
        && rm -rf /var/lib/apt/lists/* \
  )

WORKDIR /app
COPY --from=build /src/docker_build/agentd /usr/local/bin/agentd

EXPOSE 8123
CMD ["/usr/local/bin/agentd"]
