FROM debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        libcurl4-openssl-dev \
        libfreetype6-dev \
        libjson-c-dev \
        libxml2-dev \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /project
