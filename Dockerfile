FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      g++ cmake make ca-certificates && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j"$(nproc)"

FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 && \
    rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/minivectordb /usr/local/bin/minivectordb
RUN mkdir -p /data
ENTRYPOINT ["/usr/local/bin/minivectordb", "--data", "/data"]
