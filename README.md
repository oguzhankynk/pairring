# pairring

**A point-to-point message channel over memory you can write remotely.**

pairring connects two endpoints with a pair of rings: each direction is a ring
in the receiver's memory that only the sender writes. It sits between a layer
that moves bytes into remote memory (shared memory, a device window, RDMA, a
middleware of your own) and whatever produces your messages (any serializer).
It knows neither: the layer below is a five-function interface, and messages
are plain byte strings.

- Header-only C++17. No heap and no exceptions after construction, no threads,
  nothing blocks.
- Variable-size ("pipe") or fixed-size ("slot") records: one ring design, one
  geometry setting.
- Every record carries a CRC-32C; corrupt records never reach the application.
- Handshake with epochs: either side can restart and reconnect; stale data
  from an earlier session is never read.
- Doorbells are hints: one wakeup delivers everything that arrived, however
  many doorbells stand behind it.

```
   endpoint A                                   endpoint B
 ┌──────────────────────────┐                 ┌──────────────────────────┐
 │ A's region (A reads)     │                 │ B's region (B reads)     │
 │  rx ring  ◄──── B writes │                 │  rx ring  ◄──── A writes │
 │  rx_tail  ◄──── B        │   send()        │  rx_tail  ◄──── A        │
 │  tx_head  ◄──── B        │ ──────────────► │  tx_head  ◄──── A        │
 └──────────────────────────┘   drain()       └──────────────────────────┘
```

No endpoint ever reads the other's memory: each reads only its own region.

## Quick example

```cpp
#include <pairring/pairring.hpp>

// `mine`: this endpoint's region; `theirs`: the peer's region, mapped here.
pairring::Config cfg;                                  // 64 B granules, 1 MiB ring
pairring::MappedRemoteRegion remote(theirs, size, ring_doorbell, &bell);
pairring::Channel ch({mine, size}, remote, cfg);

while (ch.poll() != pairring::State::kEstablished) { wait_for_doorbell(); }

ch.send(msg, len);                                     // all or nothing; false if full
ch.drain([](const void* payload, std::size_t len) {    // zero-copy, every pending message
  handle(payload, len);
});
```

`examples/shm_pingpong.cpp` runs two processes over POSIX shared memory.

## Plugging in the layer below

Implement `pairring::RemoteRegion`; give the channel your local region as a
pointer and a size.

| Function | Required | Contract |
|---|---|---|
| `write(off, src, len)` | yes | Bulk copy into the peer's region. No atomicity, no ordering. |
| `writev(off, pieces, n)` | no | Pieces back to back. Default: one `write` per piece. |
| `store64(off, v)` | yes | 8-aligned, single-copy atomic, release: earlier operations visible no later than `v`. |
| `notify()` | yes | Doorbell, after earlier operations. Sticky: not lost if the peer is not waiting. |
| `size()` | yes | Size of the peer's region. |

When the peer's region is mapped into your process, `MappedRemoteRegion` is
ready to use; you only supply the doorbell (an eventfd, a futex, an interrupt),
or nothing, and let the peer poll.

Creating and connecting regions stays with the layer below. A local region
needs `pairring::region_size(config)` bytes.

## Feeding a serializer

Messages are byte strings, so anything that encodes into a buffer and decodes
from one fits:

- `send(buf, len)` returns `true` or `false`, never a partial message.
- `drain(fn)` hands every pending message to `fn` in place, without copying.
- `receive(buf, cap)` copies one message out; `next_size()` tells its length first.

A transport-style adapter is a few lines: `send` maps to `send`, `receive` to `receive`.

## Configuration

```cpp
struct Config {
  std::uint32_t granule_size  = 64;     // power of two, >= 16
  std::uint32_t granule_count = 16384;  // power of two, >= 2
  std::uint32_t max_message   = 4096;   // must fit in half the ring
};
```

- **Pipe behavior**: small granules; a message takes as many as it needs.
- **Slot behavior**: `granule_size >= 8 + max_message`; one message per granule.
  A corrupt message is then dropped and the session continues; with
  multi-granule records it ends the session, because a corrupt length hides
  where the next record starts.

Each endpoint configures the ring it receives on; the two directions may differ.

## Sessions

`poll()` (also called by `drain()` and `receive()`) runs the handshake. A
channel is `kHandshaking`, `kEstablished`, `kBroken` (the layer below failed or
the ring was corrupt) or `kIncompatible` (another protocol version). `restart()`
begins a new session; a peer that restarts is detected by its new epoch.
Within a session messages arrive once and in order; messages in flight when a
session ends are dropped.

The full protocol, byte for byte: [`docs/protocol.md`](docs/protocol.md).

## Threads

One thread may call `send()` while another receives (`poll`, `drain`,
`receive`, `next_size`, `restart`). A single thread may do both, and may call
`send()` from inside a `drain()` callback.

## Building and testing

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

Other presets: `release`, `asan`, `tsan`, `coverage`, `tidy`, `cppcheck`.
`scripts/run_checks.sh` runs every quality gate in CI order. Enable the
pre-commit format check with `git config core.hooksPath .githooks`.

Because the library is header-only, `src/header_check.cpp` compiles every
public header on its own so that the warnings and static analyzers cover it.

Requirements: a little-endian target, a C++17 compiler (GCC 11+ / Clang 14+),
CMake 3.25+. The tests and the example use Linux (POSIX shared memory,
eventfd). Tests fetch GoogleTest via `FetchContent`.

## Layout

```
include/pairring/
  pairring.hpp        everything
  region.hpp          RemoteRegion, LocalRegion: the lower layer contract
  config.hpp          Config, region_size(), layout
  channel.hpp         Channel, State, Stats
  mapped_region.hpp   MappedRemoteRegion
  crc32c.hpp          CRC-32C
docs/protocol.md      the protocol
src/                  generated version header, header self-check
tests/unit/           unit, protocol, thread and two-process tests
examples/             shm_pingpong
```

## License

MIT, see `LICENSE`.
