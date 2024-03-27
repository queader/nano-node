FROM ubuntu:22.04 as builder

ARG COMPILER=gcc

# Install build dependencies
COPY ./ci/prepare/linux /tmp/prepare
RUN /tmp/prepare/prepare.sh

COPY ./ /tmp/src
WORKDIR /tmp/src

# Define ARGs for ci/build-node.sh
ARG BUILD_TYPE=Debug
ARG COVERAGE=OFF
ARG SANITIZER
ARG CI_TAG=DEV_BUILD
ARG CI_VERSION_PRE_RELEASE=OFF

# Build tests
RUN ci/build-tests.sh

FROM ubuntu:22.04

# Install python
RUN apt-get update && apt-get install -y python3 python3-pip

COPY --from=builder /tmp/src/build/ /root/nano_tests
COPY --from=builder /tmp/src/submodules/gtest-parallel /root/gtest-parallel
RUN ldconfig

WORKDIR /root/nano_tests
USER root

# ENV PATH="${PATH}:/usr/bin"
# ENV PATH="${PATH}:/root/out"

ENTRYPOINT ["/root/gtest-parallel/gtest-parallel"]
CMD ["./core_test"]