# fledge-south-ethernetip

A Fledge south plugin that reads and writes data from Allen-Bradley / Rockwell PLCs via the EtherNet/IP protocol. It wraps the [libplctag](https://github.com/libplctag/libplctag) C library and presents data as Fledge `Reading` objects.

## Documentation

The full configuration reference, tag map format, troubleshooting guide and example
configurations are in [`docs/index.rst`](docs/index.rst).

## Supported PLCs

- Allen-Bradley ControlLogix / Logix5000
- Allen-Bradley Micro800
- Allen-Bradley MicroLogix
- Allen-Bradley SLC 500 (including SLC 5/05)
- Omron NJ/NX

## How It Works

The plugin runs as an **asynchronous** Fledge south plugin. On startup it launches a background poll thread that reads configured PLC tags at a configurable interval and pushes readings into the Fledge pipeline via the ingest callback.

### Tag Creation

Tags are created asynchronously in a background thread so the poll loop is never blocked by PLC connection delays. The plugin tracks tag state through a state machine:

```
configure() called         -> NEEDS_CREATION
createTagsBackground() end -> READY / PARTIAL / CREATION_FAILED
doCleanUp() called         -> NEEDS_CREATION
```

Failed tags are retried automatically, with the retry interval backing off from 1s to a
30s ceiling while the PLC stays unreachable. Sustained failures are reported as a single
rate-limited outage event rather than once per tag per cycle - see
[Outage Reporting](#outage-reporting).

### Read Modes

- **Scatter-gather** (default) - Issues `plc_tag_read(handle, 0)` for all tags simultaneously, then waits for completion with per-tag timeout tracking. Tags that time out are skipped individually without blocking others.
- **Sequential** - Reads each tag one at a time with a blocking `plc_tag_read(handle, timeout)`. Available as a fallback for PLCs that reject concurrent requests.

### Reading Structure

- **Reading per asset** (default) - All tags are bundled into a single `Reading` with multiple
  datapoints, so one poll cycle yields one reading per asset: `time, asset, tag1, tag2, tag3, …`
- **Reading per PLC tag** - Each tag produces a separate `Reading` object, so one poll cycle
  yields one reading per tag, each carrying a single datapoint.

### Write Support

The plugin exports `plugin_write` (flagged with `SP_CONTROL`). Write tags are configured via the `writemap` JSON parameter. Writes are synchronous with the configured timeout.

### ControlLogix BOOL Arrays

BOOL array tags on ControlLogix PLCs require special index arithmetic. The plugin automatically rewrites `array[N]` to `array[N/32].(N%32)`. Negative indices are rejected with an error.

## Configuration Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `address` | string | `""` | PLC IP address or hostname |
| `plc` | enumeration | `controllogix` | PLC type: `controllogix`, `logix5000`, `micro800`, `micrologix`, `slc500`, `omron-njnx` |
| `path` | string | `1,0` | CIP routing path. Spaces are stripped. Ignored for Micro800. Must be left blank for `slc500`/`micrologix` unless routing over a DH+ bridge |
| `asset` | string | `PLCTags` | Fledge asset name for emitted readings |
| `timeout` | string | `5000` | Connection and operation timeout in milliseconds (range: 1-120000) |
| `pollInterval` | string | `1000` | Poll loop interval in milliseconds |
| `readmap` | JSON | `{"tags":[]}` | JSON object with a `tags` array of tag objects to read - see [Tag Map Format](#tag-map-format) |
| `writemap` | JSON | `{"tags":[]}` | JSON object with a `tags` array of tag objects to write |
| `readingStructure` | enumeration | `Reading per asset` | `Reading per asset` or `Reading per PLC tag` |
| `readMode` | enumeration | `scatter-gather` | `scatter-gather` or `sequential` |
| `debugLevel` | enumeration | `1` | Log verbosity: 0=minimal, 1=poll summaries, 2=per-tag values, 3=full trace |
| `statsInterval` | string | `100` | Poll cycles between performance summary log lines |
| `outageLogInterval` | string | `3600` | Seconds between repeat reports while tags are unavailable; `0` logs every occurrence |

### Outage Reporting

A powered-down remote I/O cabinet is an expected operating condition, so it is
reported as one event rather than restated every poll. When no tag can be reached
the plugin logs once on the way down, once per `outageLogInterval` while it stays
down (carrying the elapsed duration), and once on recovery:

```
[OUTAGE] 10/10 read tags could not be created - still PENDING at create timeout.
         Retrying continuously; further reports at most every 1h0m until this clears.
[OUTAGE] 10/10 read tags could not be created still down 2h14m (since 2026-08-13T04:12:33Z),
         last error: still PENDING at create timeout. 812 further occurrence(s) suppressed
         since the last report.
[OUTAGE] read tag creation RECOVERED after 3h07m (down since 2026-08-13T04:12:33Z) - 10/10 OK.
```

Failures are aggregated only when *every* tag is failing, which is the signature of
a connection or power problem. If a subset fails while others read normally - usually
a wrong tag name - each tag is still named individually, rate-limited the same way.
Per-tag detail during a full outage remains available at `debugLevel` 1 and above.

Tag creation also backs off between retries, from 1s to a 30s ceiling, so an
overnight outage does not drive thousands of connect attempts. Retries never stop.

### Tag Map Format

Both `readmap` and `writemap` are JSON **objects** with a `tags` array. Each
element of the array describes one PLC tag and must have a `name` (the PLC tag
name) and a `type` (the data type):

```json
{
  "tags": [
    { "name": "MotorSpeed",   "type": "DINT" },
    { "name": "PumpPressure", "type": "REAL" },
    { "name": "AlarmFlag",    "type": "BOOL" },
    { "name": "BatchName",    "type": "STRING" },
    { "name": "Bits[33]",     "type": "BOOL" }
  ]
}
```

> The top-level `tags` object wrapper is required because Fledge `JSON`
> configuration items must be JSON objects, not bare arrays.

#### Program-scoped tags

A Logix tag that lives inside a program is identified by an optional `program`
field. An entry with `program` is program-scoped; an entry without it is
controller-scoped:

```json
{
  "tags": [
    { "name": "Level",     "type": "REAL", "program": "MainProgram" },
    { "name": "LineSpeed", "type": "REAL" }
  ]
}
```

The plugin builds the address `Program:MainProgram.Level` from the first entry.
Don't add the `Program:` prefix to `name` as well; an entry that has both is
skipped with a warning. That full address is still the default datapoint name and
the name a `writemap` tag is written by.

#### Per-tag asset and datapoint remapping (readmap only)

In the `readmap`, an entry may also carry optional `asset` and/or `datapoint`
fields that remap the tag to a specific Fledge asset and/or datapoint name:

```json
{
  "tags": [
    { "name": "MotorSpeed",   "type": "DINT" },
    { "name": "PumpPressure", "type": "REAL", "asset": "Tank1", "datapoint": "Pressure" },
    { "name": "Tank1_Level",  "type": "REAL", "asset": "Tank1", "datapoint": "Level" },
    { "name": "AlarmFlag",    "type": "BOOL", "datapoint": "Alarm" }
  ]
}
```

- `name` and `type` are **required**; an entry missing a string `name` or `type`
  is skipped with a warning.
- `asset` is optional - if omitted, the global `asset` configuration parameter is
  used.
- `datapoint` is optional - if omitted, the PLC tag name is used as the datapoint
  name.

This works in both reading structures. With **Reading per PLC tag**, each tag
becomes its own `Reading` under its (possibly remapped) asset and datapoint name.
With **Reading per asset**, datapoints are grouped by their *effective* asset
name, so the example above produces one `Tank1` reading containing both
`Pressure` and `Level` datapoints. Asset and datapoint ordering follows tag
declaration order.

> **Note:** The `asset`/`datapoint` remap fields apply to `readmap` only. The
> `writemap` accepts only `name` and `type`.

### Supported Data Types

| Type | Aliases | Description |
|------|---------|-------------|
| `BOOL` | | Single bit |
| `SINT` | `INT8` | 8-bit signed integer |
| `USINT` | `UINT8` | 8-bit unsigned integer |
| `INT` | `INT16` | 16-bit signed integer |
| `UINT` | `UINT16` | 16-bit unsigned integer |
| `DINT` | `INT32` | 32-bit signed integer |
| `UDINT` | `UINT32`, `DWORD` | 32-bit unsigned integer |
| `LINT` | `INT64` | 64-bit signed integer |
| `ULINT` | `UINT64` | 64-bit unsigned integer |
| `REAL` | `FLOAT32` | 32-bit float |
| `LREAL` | `FLOAT64` | 64-bit float |
| `STRING` | | PLC string |

#### Declared-type verification

A wrong `type` is a silent failure - libplctag reinterprets the raw bytes and
returns plausible-looking garbage rather than an error. After tag creation the
plugin compares each declared type against the CIP type the controller reports
and logs a warning per mismatch:

```
[TYPE_MISMATCH] tag='MotorSpeed' is declared as REAL but the PLC reports INT
  (CIP type 0x00C3) - values for this tag will be misinterpreted
```

The check is advisory - mismatched tags are still polled. It runs once per
creation cycle, and only on `controllogix`, `logix5000` and `micro800`;
libplctag reports no type information for `micrologix` and `omron-njnx`, so
those tags are skipped. Bit-addressed BOOLs (including ControlLogix BOOL array
elements) and strings are not flagged, since the PLC legitimately reports the
containing word's type or a family-specific string encoding.

## Dependencies

| Library | Role | Link type |
|---------|------|-----------|
| `libplctag` (>= 2.7.2) | EtherNet/IP PLC tag I/O | Dynamic |
| Fledge SDK | `ConfigCategory`, `Reading`, `Logger`, etc. | Dynamic |
| RapidJSON | JSON parsing for tag maps | Header-only |

`libplctag` is licensed under MPL-2.0 (or LGPL-2.0+) and is bundled unmodified in
the package. See [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES).

## Building

### Prerequisites

- CMake >= 3.6
- C++17 compiler (GCC >= 7 or Clang >= 5)
- Fledge source tree or installed Fledge development headers
- libplctag development package

### Build the Plugin

```bash
mkdir build && cd build
cmake -DFLEDGE_ROOT=/path/to/fledge ..
make
```

The output is `libethernetip.so` in the build directory.

### Install

```bash
sudo make install
```

This copies `libethernetip.so` to `$FLEDGE_ROOT/plugins/south/ethernetip/`.

### Convenience Makefile

A top-level `Makefile` wraps the common CMake workflows so they can be driven
with plain `make`:

```bash
make                 # build libethernetip.so
make install         # install into $FLEDGE_ROOT/plugins/south
make test            # build and run the test suites
make clean           # remove build/ and packaging output
make FLEDGE_ROOT=/path/to/fledge   # forward FLEDGE_ROOT to cmake
```

## Packaging

The Debian package is built by the shared Fledge plugin workflow, which
`.github/workflows/build-deb.yml` calls on every pull request and on pushes to
`develop`. The package installs `libethernetip.so` into
`/usr/local/fledge/plugins/south/ethernetip/` and bundles `libplctag` into
`/usr/local/fledge/lib/`, so no separate libplctag install is required on the
target.

## Testing

The project includes four test suites built with Google Test. Tests are enabled with the `BUILD_TESTS` cmake flag.

### Build Tests

```bash
mkdir build && cd build
cmake -DFLEDGE_ROOT=/path/to/fledge -DBUILD_TESTS=ON ..
make
```

### Unit Tests (58 tests)

Tests the `PLCTag` class in isolation using a link-time mock of libplctag. No PLC or network access required.

```bash
./tests/unit_tests
```

Coverage includes:
- Configuration parsing (readmap, writemap, path stripping, Micro800 special case)
- Change detection and cleanup
- Timeout validation (valid range, zero, overflow, non-numeric)
- ControlLogix BOOL array index rewriting (indices 0, 31, 32, 33)
- Tag state machine (creation, failure, retry)
- Read value conversion for DINT, REAL, BOOL, STRING types
- Reading structure modes (per-tag vs per-asset)
- Per-tag asset/datapoint remapping (full/partial overrides, per-asset grouping, malformed-entry skipping)
- Write operations (success, unknown tag, write failure)
- Read modes (scatter-gather vs sequential)
- Async plugin lifecycle (start/stop, ingest callback)

### Plugin Load Tests (15 tests)

Uses `dlopen` to load the built `libethernetip.so` and verify Fledge can load it correctly. No PLC required.

```bash
./tests/plugin_load_tests
```

Coverage includes:
- Shared library loads without missing symbols
- All 8 plugin entry points resolve (`plugin_info`, `plugin_init`, `plugin_start`, `plugin_poll`, `plugin_reconfigure`, `plugin_shutdown`, `plugin_write`, `plugin_register_ingest`)
- `plugin_info()` returns correct metadata (name, type, flags)
- Default configuration JSON is valid and contains all required keys
- `plugin_init` / `plugin_shutdown` round-trip without crash
- Full async lifecycle (init, register ingest, start, shutdown)

### Integration Tests (21 tests)

Tests against a real or virtual PLC over the network using the real libplctag library.

```bash
VPLC_ADDRESS=192.168.1.10 ./tests/integration_tests
```

Coverage includes:
- Connection to PLC
- Reading all 8 data types (DINT, REAL, BOOL, STRING, INT, LREAL, SINT, LINT)
- Write and read-back verification
- Reading-per-asset mode
- Sequential read mode
- Repeated polling stability (20 rapid polls)
- Async ingest callback delivery
- Reconfiguration and reconnection
- Graceful failure on unreachable address

#### Integration Test Configuration

All settings are controlled via environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| `VPLC_ADDRESS` | `127.0.0.1` | PLC IP address or hostname |
| `VPLC_PATH` | `""` | CIP path (empty = none) |
| `VPLC_PLC_TYPE` | `controllogix` | PLC type string |
| `VPLC_TIMEOUT` | `5000` | Timeout in milliseconds |
| `VPLC_READMAP` | Built-in default | JSON readmap override |
| `VPLC_WRITEMAP` | Built-in default | JSON writemap override |

Default read tags expected on the PLC: `TestDINT` (DINT), `TestREAL` (REAL), `TestBOOL` (BOOL), `TestSTRING` (STRING), `TestINT` (INT), `TestLREAL` (LREAL), `TestSINT` (SINT), `TestLINT` (LINT).

Default write tags: `WriteDINT` (DINT), `WriteREAL` (REAL), `WriteBOOL` (BOOL), `WriteSTRING` (STRING).

Override with `VPLC_READMAP` and `VPLC_WRITEMAP` to match your PLC's tag configuration.

### SLC 500 Tests (10 tests)

Integration tests for the PCCC path, run against a virtual SLC 500 rather than
real hardware. The simulator is `ab_server` from the libplctag source tree,
fetched and built on demand by `tests/vplc/vplc.sh` - see
[`tests/vplc/README.md`](tests/vplc/README.md).

```bash
make test-slc500          # build, start the simulator, run, stop it
```

Or manage the simulator yourself:

```bash
./tests/vplc/vplc.sh start
./build/tests/slc500_tests
./tests/vplc/vplc.sh stop
```

Every test skips itself when nothing is listening, so `make test` stays green
on a machine that has never started the simulator.

These cover what the ControlLogix suite cannot: tag creation with **no CIP
path**, PCCC data file addressing (`N7:0`, `F8:0`, `B3:0/1`, `L19:0`,
`ST18:0`), and the PCCC masked bit write. Because the simulator starts with
every data file zeroed, each test writes a known value and reads it back, so
the write path is covered alongside the read path.

| Variable | Default | Meaning |
|---|---|---|
| `VPLC_SLC_ADDRESS` | `127.0.0.1:44819` | Simulator address |
| `VPLC_SLC_PORT` | `44819` | Port `vplc.sh` starts the simulator on |
| `VPLC_SLC_TIMEOUT` | `5000` | Timeout in milliseconds |

## Logging

All log lines include a context prefix for multi-instance identification:

```
[EtherIP|<asset>|<address>] <message>
```

Error messages include a classification tag for log-based triage:

| Tag | Meaning |
|-----|---------|
| `[CONFIG_ERR]` | Bad configuration (empty address, unknown type, invalid tag name) |
| `[CONN_ERR]` | Network-level failure (bad gateway, connection refused) |
| `[TAG_ERR]` | Tag not found or access denied on PLC |
| `[TIMEOUT]` | Operation exceeded its deadline |
| `[DATA_ERR]` | Type mismatch, value out of range |

Performance statistics are logged every `statsInterval` polls:

```
[EtherIP|PLCTags|192.168.1.10] poll stats [last 100 polls]: avgTotal=14.3ms avgScatter=0.1ms avgGather=13.8ms min/maxGather=11.2/48.7ms avgCollect=0.4ms successRate=100.0% timeouts=0
```

## Project Structure

```
fledge-south-ethernetip/
  .github/workflows/
    build-deb.yml             # CI: calls the shared Fledge plugin .deb workflow
  CMakeLists.txt              # Build system (plugin + optional tests)
  Package                     # Packaging metadata (name, type, requirements)
  Description                 # One-line package description
  VERSION                     # Plugin version
  fledge.version              # Minimum required Fledge version
  requirements.sh             # Builds libplctag from source if absent
  mkversion                   # Generates version.h at build time
  plugin.cpp                  # C-linkage entry points, default config JSON
  plctag.cpp                  # PLCTag class implementation
  include/
    plctag.h                  # PLCTag class declaration, enums, structs
  tests/
    CMakeLists.txt            # Test build (included via BUILD_TESTS=ON)
    mock_libplctag.h          # GMock class for libplctag C API
    mock_libplctag.cpp        # Link-time mock implementation
    unit/
      test_plctag.cpp         # Unit tests (58 tests)
      test_plugin_load.cpp    # Plugin load tests (15 tests)
    integration/
      vplc_config.h           # Virtual PLC configuration and defaults
      test_integration.cpp    # Integration tests (21 tests)
      slc500_config.h         # Virtual SLC 500 configuration
      test_slc500.cpp         # SLC 500 integration tests (10 tests)
    vplc/
      vplc.sh                 # Build/start/stop the virtual SLC 500
      README.md               # Simulator notes and data file map
  docs/
    index.rst               # Full plugin documentation
```
