#!/bin/bash
set -euox pipefail
shopt -s nocasematch  # Enable case-insensitive matching

BUILD_TARGET=""
if [[ ${1:-} ]]; then
    BUILD_TARGET="--target $1"
fi

SRC=${SRC:-${PWD}}
OS=$(uname)

CMAKE_BACKTRACE=""
if [[ "$OS" == 'Linux' ]]; then
    CMAKE_BACKTRACE="-DNANO_STACKTRACE_BACKTRACE=ON"

    if [[ "$COMPILER" == 'clang' ]]; then
        CMAKE_BACKTRACE="${CMAKE_BACKTRACE} -DNANO_BACKTRACE_INCLUDE=</tmp/backtrace.h>"
    fi
fi

CMAKE_QT_DIR=""
if [[ ${QT_DIR:-} ]]; then
    CMAKE_QT_DIR="-DQt5_DIR=${QT_DIR}"
fi

CMAKE_SANITIZER=""
if [[ ${SANITIZER:-} ]]; then
    case "${SANITIZER}" in
        ASAN)     
            CMAKE_SANITIZER="-DNANO_ASAN=ON"
            ;;
        ASAN_INT)    
            CMAKE_SANITIZER="-DNANO_ASAN_INT=ON"
            ;;
        TSAN)
            CMAKE_SANITIZER="-DNANO_TSAN=ON"
            ;;
        UBSAN)
            CMAKE_SANITIZER="-DNANO_UBSAN=ON"
            ;;
        LEAK)
            CMAKE_SANITIZER="-DNANO_ASAN=ON"
            ;;
        *)
            echo "Unknown sanitizer: '${SANITIZER}'"
            exit 1
            ;;
    esac
fi

BUILD_DIR="build"

mkdir -p $BUILD_DIR
pushd $BUILD_DIR

cmake \
-DCMAKE_BUILD_TYPE=${BUILD_TYPE:-"Debug"} \
-DPORTABLE=ON \
-DACTIVE_NETWORK=nano_${NANO_NETWORK:-"live"}_network \
-DNANO_TEST=${NANO_TEST:-OFF} \
-DNANO_GUI=${NANO_GUI:-OFF} \
-DNANO_TRACING=${NANO_TRACING:-OFF} \
-DCOVERAGE=${COVERAGE:-OFF} \
-DCI_TAG=${CI_TAG:-OFF} \
-DCI_VERSION_PRE_RELEASE=${CI_VERSION_PRE_RELEASE:-OFF} \
${CMAKE_SANITIZER:-} \
${CMAKE_QT_DIR:-} \
${CMAKE_BACKTRACE:-} \
${SRC}

number_of_processors() {
    local max_procs=$(nproc)
    case "$(uname -s)" in
        Linux*)
            max_procs=$(nproc)
            ;;
        Darwin*)
            max_procs=$(sysctl -n hw.ncpu)
            ;;
        CYGWIN*|MINGW32*|MSYS*|MINGW*)
            max_procs="${NUMBER_OF_PROCESSORS}"
            ;;
        *)
            echo "Unknown OS"
            exit 1
            ;;
    esac

    # If MAX_BUILD_PARALLELISM is set and less than the number of processors, use it instead
    if [[ ! -z ${MAX_BUILD_PARALLELISM:-} ]] && [[ "$MAX_BUILD_PARALLELISM" -lt "$max_procs" ]]; then
        echo "$MAX_BUILD_PARALLELISM"
    else
        echo "$max_procs"
    fi
}

parallel_build_flag() {
    case "$(uname -s)" in
        CYGWIN*|MINGW32*|MSYS*|MINGW*)
            echo "-- -m"
            ;;
        *)
            echo "--parallel $(number_of_processors)"
            ;;
    esac
}

cmake --build ${PWD} ${BUILD_TARGET} $(parallel_build_flag)

popd