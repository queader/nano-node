# See here for image contents: https://github.com/microsoft/vscode-dev-containers/tree/v0.209.6/containers/cpp/.devcontainer/base.Dockerfile

# [Choice] Debian / Ubuntu version (use Debian 11/9, Ubuntu 18.04/21.04 on local arm64/Apple Silicon): debian-11, debian-10, debian-9, ubuntu-21.04, ubuntu-20.04, ubuntu-18.04
ARG VARIANT="bullseye"
FROM mcr.microsoft.com/vscode/devcontainers/cpp:0-${VARIANT}

# [Optional] Uncomment this section to install additional packages.
# RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
#     && apt-get -y install --no-install-recommends <your-package-list-here>

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq && apt-get install -yqq \
build-essential \
g++ \
wget \
python \
zlib1g-dev

RUN wget -O cmake_install.sh https://github.com/Kitware/CMake/releases/download/v3.15.4/cmake-3.15.4-Linux-x86_64.sh && \
chmod +x cmake_install.sh && \
./cmake_install.sh --prefix=/usr --exclude-subdir --skip-license

RUN apt-get update -qq && apt-get install -yqq \
qt5-default \
valgrind \
xorg xvfb xauth xfonts-100dpi xfonts-75dpi xfonts-scalable xfonts-cyrillic

RUN apt update -qq && apt-get install -yqq git

ENV BOOST_ROOT=/tmp/boost

ADD ../util/build_prep/fetch_boost.sh fetch_boost.sh

RUN COMPILER=gcc ./fetch_boost.sh