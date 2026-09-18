FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN chmod 1777 /tmp && apt-get update && apt-get install -y --no-install-recommends g++-14 cmake make python3 git libfuse3-dev fuse3 pkg-config ca-certificates && rm -rf /var/lib/apt/lists/*
ENV CC=gcc-14 CXX=g++-14
WORKDIR /work
CMD ["bash"]
