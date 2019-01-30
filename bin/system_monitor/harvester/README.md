# System Monitor Harvester

The Harvester runs on the Fuchsia device, acquiring Samples (units of
introspection data) that it sends to the Host using the Transport system.

The Harvester should not unduly impact the Fuchsia device being monitored.
So the Harvester does not store samples. Instead the samples are moved to
the Dockyard as soon as reasonable.

## qemu

On the host, run
$ fx run -N -u $FUCHSIA_DIR/scripts/start-dhcp-server.sh

Tip: If you're doing edit-compile-run development, you might prefer this:
$ killall -r qemu-; fx build && fx run -N -u $FUCHSIA_DIR/scripts/start-dhcp-server.sh
