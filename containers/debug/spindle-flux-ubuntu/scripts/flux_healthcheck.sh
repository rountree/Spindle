#!/bin/bash

FLUX_FREE_NODES=$(flux resource list -s free -n | awk '{print $2}')

if [[ ${FLUX_FREE_NODES} -ne ${replicas} ]] ; then
    echo "FAILED: Incorrect number of Flux free nodes: expected ${replicas}, got ${FLUX_FREE_NODES}"
    exit 1
fi

echo "PASSED: Found ${FLUX_FREE_NODES} nodes out of ${replicas}; all nodes registered."

