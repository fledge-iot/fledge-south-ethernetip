# Virtual SLC 500

An integration-test target that speaks real EtherNet/IP over a real socket, so
the plugin's PCCC path can be exercised without an SLC on the bench.

## What it is

`vplc.sh` wraps **ab_server**, the reference PLC simulator that ships in the
[libplctag](https://github.com/libplctag/libplctag) source tree. That matters:
ab_server comes from the same project as the client library the plugin links
against, so its PCCC wire format is authoritative. If the plugin talks to this,
it talks to a real SLC.

The simulator is fetched and built on first use - it is not vendored. The
version is pinned to the same libplctag release the plugin links against
(2.7.2, matching `requirements.sh`). ab_server does not link libplctag itself,
so the script compiles its ~15 source files directly rather than running
libplctag's full CMake build.

## Usage

```bash
./tests/vplc/vplc.sh build     # fetch + compile (cached under build/vplc/)
./tests/vplc/vplc.sh start     # run it in the background
./tests/vplc/vplc.sh status
./tests/vplc/vplc.sh log       # tail the simulator's own log
./tests/vplc/vplc.sh stop
```

Or let make drive the whole cycle:

```bash
make test-slc500               # build tests, start sim, run, stop sim
```

The tests skip themselves when nothing is listening, so a plain `make test` on
a machine that has never run the simulator still passes.

## Data files

ab_server's PCCC implementation supports five data files, which between them
cover every SLC type the plugin maps:

| File   | Element         | Plugin type | Example address |
|--------|-----------------|-------------|-----------------|
| `B3`   | 16-bit bit word | `BOOL`      | `B3:0/1`        |
| `N7`   | 16-bit integer  | `INT`       | `N7:0`          |
| `F8`   | 32-bit float    | `REAL`      | `F8:0`          |
| `L19`  | 32-bit integer  | `DINT`      | `L19:0`         |
| `ST18` | 82-char string  | `STRING`    | `ST18:0`        |

Timer, counter and status files (`T4:0.ACC`, `C5:0.ACC`, `S:1`) are **not**
simulated - ab_server does not implement them. Those addresses have to be
verified against real hardware.

## Configuration

| Variable | Default | Meaning |
|---|---|---|
| `VPLC_SLC_PORT` | `44819` | Listen port. Not 44818, because a ControlLogix vPLC is often already using it. |
| `VPLC_SLC_ADDRESS` | `127.0.0.1:44819` | What the test binary connects to. libplctag accepts `host:port` in its gateway attribute. |
| `LIBPLCTAG_VERSION` | `2.7.2` | Release to build ab_server from. |

## Notes

- Every data file starts **zeroed**. The tests write a known value and read it
  back rather than asserting against pre-loaded contents - which also means
  they cover `plugin_write` on the PCCC path, not just reads.
- `make clean` removes `build/`, which includes the cached libplctag checkout,
  so the next run re-fetches it.
- The simulator serves one client at a time and does not pack requests, which
  is faithful to a real SLC: PCCC has no multi-request service.
