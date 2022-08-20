FROM nanocurrency/nano-env:gcc

ARG CI_TAG=DEV_BUILD
ARG CI_BUILD=OFF
ADD ./ /tmp/src

RUN mkdir /tmp/build && \
cd /tmp/build && \
cmake /tmp/src \
-DCMAKE_BUILD_TYPE=Debug \
-DCI_BUILD=${CI_BUILD} \
-DBOOST_ROOT=${BOOST_ROOT} \
-DNANO_TEST=ON && \
make core_test -j $(nproc)
# make slow_test -j $(nproc) && \
# make rpc_test -j $(nproc) && \
# make load_test -j $(nproc)

FROM ubuntu:18.04

RUN groupadd --gid 1000 nanocurrency && \
useradd --uid 1000 --gid nanocurrency --shell /bin/bash --create-home nanocurrency

COPY --from=0 /tmp/build/core_test /usr/bin
# COPY --from=0 /tmp/build/rpc_test /usr/bin
# COPY --from=0 /tmp/build/slow_test /usr/bin
# COPY docker/tests/entry.sh /usr/bin/entry.sh
# COPY --from=0 /tmp/boost/lib/* /usr/local/lib/
# RUN chmod +x /usr/bin/entry.sh
# RUN ldconfig

WORKDIR /root
USER root

ENV PATH="${PATH}:/usr/bin"
# ENTRYPOINT ["/usr/bin/entry.sh"]
ENTRYPOINT ["/usr/bin/core_test"]
