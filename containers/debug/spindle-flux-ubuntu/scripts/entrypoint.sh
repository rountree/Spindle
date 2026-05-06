#!/bin/bash

# Starts munged and the flux broker.
#
# For documentation on running Flux in containers, see
# https://flux-framework.readthedocs.io/en/latest/tutorials/containers

brokerOptions="-Scron.directory=/etc/flux/system/cron.d \
  -Stbon.fanout=256 \
  -Srundir=/run/flux \
  -Sstatedir=${STATE_DIRECTORY:-/var/lib/flux} \
  -Slocal-uri=local:///run/flux/local \
  -Slog-stderr-level=6 \
  -Slog-stderr-mode=local"

# Get the hostname that will resolve for the Docker bridge network.
echo "=== Entrypoint Debug ==="
echo "hostname: $(hostname)"
echo "hostname -i: $(hostname -i)"
address=$(echo $( nslookup "$( hostname -i )" | head -n 1 ))
echo "nslookup result: $address"
parts=(${address//=/ })
hostName=${parts[2]}
echo "parsed hostName: $hostName"
thisHost=(${hostName//./ })
thisHost=${thisHost[0]}
echo "thisHost (first part): $thisHost"
export FLUX_FAKE_HOSTNAME=$thisHost
echo "FLUX_FAKE_HOSTNAME: $FLUX_FAKE_HOSTNAME"
echo "mainHost: $mainHost"
echo "broker.toml contents:"
cat /etc/flux/config/broker.toml
echo "=== End Debug ==="

# Start munged
sudo -u munge /usr/sbin/munged

if [ ${thisHost} != "${mainHost}" ]; then
    # Worker node -- wait for head node before connecting
    sleep 15
    FLUX_FAKE_HOSTNAME=$thisHost flux start -o --config /etc/flux/config ${brokerOptions} sleep inf
else
    # Head node
    FLUX_FAKE_HOSTNAME=$thisHost flux start -o --config /etc/flux/config ${brokerOptions} sleep inf
fi

