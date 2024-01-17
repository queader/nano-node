#!/bin/bash
set -euox pipefail

NANO_GUI=ON \
$(dirname "$BASH_SOURCE")/build.sh package