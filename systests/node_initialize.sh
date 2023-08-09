#!/bin/bash
set -eux

test_cmd() {
    netmatch="$1"
    netcmd="$2"
    netarg="$3"
    genesishash="$4"

    DATADIR=$(mktemp -d)

    # initialise data directory and check that it is the correct network
    # do not use -q flag, otherwise node could terminate early (SIGPIPE)
    $NANO_NODE_EXE --initialize --data_path "$DATADIR" "$netcmd" "$netarg" | grep "Active network: $netmatch"

    # check that the ledger file is created and has one block, the genesis block
    $NANO_NODE_EXE --debug_block_count --data_path "$DATADIR" "$netcmd" "$netarg" | grep -q 'Block count: 1'

    # check the genesis block is correct
    $NANO_NODE_EXE --debug_block_dump --data_path "$DATADIR" "$netcmd" "$netarg" | head -n 1 | grep -qi "$genesishash"
}

test_cmd "live" "--network" "live" "991CF190094C00F0B68E2E5F75F6BEE95A2E0BD93CEAA4A6734DB9F19B728948"
test_cmd "beta" "--network" "beta" "01A92459E69440D5C1088D3B31F4CA678BE944BAB3776C2E6B7665E9BD99BD5A"
test_cmd "test" "--network" "test" "B1D60C0B886B57401EF5A1DAA04340E53726AA6F4D706C085706F31BBD100CEE"

# if it got this far then it is a pass
exit 0
