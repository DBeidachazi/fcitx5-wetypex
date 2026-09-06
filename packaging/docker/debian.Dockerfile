FROM debian:trixie-slim

RUN apt-get update -qq && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
      build-essential cmake clang git pkg-config libboost-dev \
      libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev \
      fcitx5-modules-dev libimecore-dev libimepinyin-dev \
      libjson-c-dev libcurl4-openssl-dev libc++-dev libc++abi-dev \
      qt6-base-dev qt6-svg-dev qt6-webengine-dev && \
    rm -rf /var/lib/apt/lists/*
