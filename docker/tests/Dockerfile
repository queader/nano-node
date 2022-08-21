ARG ENV_REPOSITORY=nanocurrency/nano-env
ARG COMPILER=gcc
FROM ${ENV_REPOSITORY}:${COMPILER}

ARG CI_TAG=DEV_BUILD
ARG CI_BUILD=OFF
ADD ./ /tmp/src

WORKDIR /build

RUN \
cmake /tmp/src \
-DCMAKE_BUILD_TYPE=Debug \
-DCI_BUILD=${CI_BUILD} \
-DNANO_TEST=ON

RUN \
make core_test -j $(nproc)
# make slow_test -j $(nproc) && \
# make rpc_test -j $(nproc) && \
# make load_test -j $(nproc)

ENTRYPOINT ["/build/core_test"]
