# fty-outage

Agent fty-outage produces pure alerts on \_ALERTS\_SYS when no data are coming from the device.

## How to build

To build fty-outage project run:

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=usr -DBUILD_TESTING=On ..
make
sudo make install
```

Optionally, to run fty-outage tests run:

```bash
make test
```

## How to run

To run fty-outage project:

* from within the source tree, run:

```bash
./src/fty-outage
```

For the other options available, refer to the manual page of fty-outage

* from an installed base, using systemd, run:

```bash
systemctl start fty-outage
```

### Configuration file

Configuration file - fty-outage.cfg - is currently ignored.

Agent reads environment variable BIOS\_LOG\_LEVEL which controls verbosity level.

State file for fty-outage is stored in /var/lib/fty/fty-outage.zpl.

## Architecture

### Overview

fty-outage is composed of 1 actor and 2 timers.

* fty-outage-server: main actor

First timer is implemented via checking zclock and saves the state of the agent each SAVE\_INTERVAL\_MS milliseconds (default value 45 minutes).

Second timer is implemented via zpoller timeout and publishes outage alerts for dead devices every TIMEOUT\_MS milliseconds (default value 30 seconds) unless such an alert is already active.

## Protocols

### Published metrics

Agent doesn't publish any metrics.

### Published alerts

Agent publishes alerts on \_ALERTS\_SYS stream.

### Mailbox requests

It is possible to request the agent fty-outage for:

* putting devices into or returning devices from maintenance mode: this is used
to temporarily ignore outages on assets that are known to not be currently
serving data (for example, due to a FW upgrade).

#### Putting devices into or returning devices from maintenance mode

The USER peer sends the following messages using MAILBOX SEND to
FTY-OUTAGE-AGENT ("fty-outage") peer:

* REQUEST/'correlation\_ID'/MAINTENANCE_MODE/<mode>/asset1/.../assetN/expiration_ttl - switch 'asset1' to 'assetN' into maintenance

where
* '/' indicates a multipart string message
* 'correlation\_ID' is a zuuid identifier provided by the caller
* <mode> MUST be 'enable' or 'disable'
* 'asset1', ..., 'assetN' MUST be the device(s) asset name
* 'expiration_ttl' (optional) is an amount of seconds after which the asset(s)
will be automatically returned from maintenance mode. If 'expiration_ttl' is not
provided, the default value ('maintenance_expiration') will be used from agent
configuration file
* subject of the message is discarded

The FTY-OUTAGE-AGENT peer MUST respond with one of the messages back to USER
peer using MAILBOX SEND.

* REPLY/correlation\_ID/OK
* REPLY/correlation\_ID/ERROR/reason

where
* '/' indicates a multipart frame message
* 'correlation ID' is a zuuid identifier provided by the caller
* 'reason' is string detailing reason for error. Possible values are:
  * Invalid command,
  * Invalid message type,
  * Command failed,
  * Missing maintenance mode,
  * Unsupported maintenance mode.


### Stream subscriptions

Agent is subscribed to streams METRICS, METRICS\_UNAVAILABLE, METRICS\_SENSOR and ASSETS.

If it gets METRICS\_UNAVAILABLE message, it resolves all the stored alerts for specified device.

If it gets METRICS or METRICS\_SENSOR message from a device, it resolves all the stored alerts for specified device and marks the device as active.

If it gets ASSETS message, it updates the asset cache. If the message is for operation DELETE or RETIRE, it resolves all the alerts for specified device.
