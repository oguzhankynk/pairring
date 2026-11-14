# pairring protocol, version 1

A point-to-point, bidirectional message channel between two endpoints over a
memory that each side can write remotely and read locally. Each direction is a
single-producer/single-consumer ring of fixed-size granules that lives in the
receiver's memory.

Header-only C++17. No heap and no exceptions after initialization.

## 1. Lower layer contract

Each endpoint gets two things from the layer below:

- **Local region**: memory this endpoint reads with plain loads (`base`, `size`).
  Only the peer writes into it.
- **Remote region**: a handle to the peer's local region with these operations:

| Operation | Required | Semantics |
|---|---|---|
| `write(off, src, len)` | yes | Bulk copy. No atomicity, no ordering against other writes. |
| `writev(off, pieces, n)` | no | Writes the pieces back to back starting at `off`. Default: one `write` per piece. |
| `store64(off, v)` | yes | `off` is 8-aligned. Single-copy atomic. Release: every earlier operation on this handle is visible to the peer no later than `v`. |
| `notify()` | yes | Doorbell. Every earlier operation is visible before the peer sees it. **Sticky**: a doorbell raised while the peer is not waiting is not lost. |
| `size()` | yes | Size of the peer's region. |

`store64` only has to order what came **before** it. The protocol never relies
on a later operation staying behind an earlier one.

Further rules:

- Ordering holds per remote handle only.
- The local side reads 64-bit control words with acquire loads.
- There are no remote reads.
- An operation that fails (peer gone, link down) returns `false`; the channel
  then goes to `Broken` (section 6).

How regions are created, published and connected is the lower layer's
business. The channel receives them ready to use.

## 2. Region layout

Every endpoint's local region, all of it written by the peer:

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 56 | `hello` | The peer's announcement (section 5) |
| 56 | 8 | `hello_commit` | Epoch that commits the announcement |
| 64 | 64 | `rx_tail` | Granules the peer has published into my rx ring |
| 128 | 64 | `tx_head` | Granules of my tx ring (in the peer's region) the peer has consumed |
| 192 | 64 | `ack` | Last epoch of mine the peer has accepted |
| 256 | G × N | rx ring | Messages from the peer |

Each control word is a `u64` on its own 64-byte line. Every word has exactly
one writer: the peer.

Required local region size: `256 + G × N`.

## 3. Ring

- **Granule size `G`**: power of two, at least 16.
- **Granule count `N`**: power of two.
- **Counters** `tail` and `head` are `u64` granule counts that only grow.
  Used space is `tail - head`; the position of a counter is `counter mod N`.
- **Record**: starts on a granule boundary and occupies
  `k = ceil((8 + len) / G)` consecutive granules. A record never wraps.

```
+---------+---------+------------------------+---------+
| len u32 | crc u32 | payload (len bytes)    | padding |
+---------+---------+------------------------+---------+
```

- `crc` is CRC-32C (Castagnoli) over the 4 bytes of `len` followed by the
  payload. All integers are little endian.
- Payloads start 8 bytes after a granule boundary, so they are 8-byte aligned.
- **Skip record**: `len = 0xFFFFFFFF`. It fills the granules from its position
  to the end of the ring; reading continues at position 0.
- **Message length**: 1 to `M` bytes. Empty messages are not supported, so that
  "no message" and "a message" are never confused.
- **Maximum message `M`**: `ceil((8 + M) / G) <= N / 2`. This guarantees that
  any message fits into an empty ring, with or without a skip record.
- **Slot behavior** is the configuration `G >= 8 + M`: every message is
  exactly one granule and skip records never occur.

## 4. Data path

### send(msg, len)

1. Not `Established`, `len == 0`, or `len > M` of the peer: return `false`.
2. `head` = acquire load of my local `tx_head`; `free = N - (tail - head)`.
3. `pos = tail mod N`. If `k > N - pos`, the record needs a skip first:
   `need = (N - pos) + k`; otherwise `need = k`.
4. `need > free`: return `false` (counted as `tx_full`).
5. If a skip is needed, write a skip header at `pos`; `tail += N - pos`; `pos = 0`.
6. `writev` the header and the payload to the ring at `pos`; `tail += k`.
7. `store64(peer.rx_tail, tail)`: this publishes the skip and the record.
8. `notify()`. Return `true`.

`send` never blocks and never returns a partial message.

### drain(callback)

```
loop:
  t = acquire load of local rx_tail
  if t == consumed: break
  while consumed < t:
    read the header at consumed mod N
    skip record:      consumed += N - pos; continue
    invalid header:   go to Broken (len 0 or > M, record past the end of the ring or past t)
    crc mismatch:     rx_crc_errors++; drop if records are single-granule, else go to Broken
    otherwise:        callback(payload, len); consumed += k
  store64(peer.tx_head, consumed)
  notify()
```

- Any number of doorbells may be merged into one wakeup: the channel takes the
  count from `rx_tail`, never from doorbells. Extra or late doorbells find
  nothing and are harmless.
- `rx_tail` is published with `store64` after the records, so every record up
  to the value read is complete.
- Head feedback and its doorbell go out once per batch, not once per message.
- The payload pointer is valid only during the callback. The callback may call
  `send` but not `drain`.

`receive(buf, cap)` copies one message out and returns its length, or 0 when
there is none. `next_size()` returns the length of the next message, so the
caller can size `buf`. A message larger than `cap` is not consumed.

A corrupted `len` makes the position of the next record unknown, so with
multi-granule records a CRC mismatch must end the session. With single-granule
records the next record is always one granule further, so the bad one is
dropped and reading continues.

## 5. Handshake and reconnect

Each endpoint has an **epoch**: a non-zero `u64` that differs from every
earlier session of that endpoint (default: wall-clock nanoseconds at start,
mixed with the address of the channel object).

`hello` block, written by an endpoint into the peer's region at offset 0:

| Offset | Field | Type |
|---|---|---|
| 0 | `magic` | `u64` = `"PAIRRING"` in memory order |
| 8 | `version` | `u32` = 1 |
| 12 | `flags` | `u32` = 0 |
| 16 | `granule_size` | `u32` |
| 20 | `granule_count` | `u32` |
| 24 | `max_message` | `u32` |
| 28 | `reserved` | `u32` |
| 32 | `ring_offset` | `u64` = 256 |
| 40 | `epoch` | `u64`, the sender's epoch |
| 48 | `crc` | `u32`, CRC-32C of bytes 0..47 |
| 52 | `pad` | `u32` |
| 56 | `hello_commit` | `u64`, equal to `epoch`, written last |

The geometry fields describe the sender's **rx** ring: the ring the receiver of
this block will write into.

A reader takes the block only when `epoch == hello_commit` and the `crc`
matches. A block read while the peer was rewriting it fails that check and is
read again on the next poll, so rewriting needs no extra ordering.

**Publishing hello** (endpoint X, peer Y), in this order:

1. `write` the 56-byte block into Y's region.
2. `store64(Y.rx_tail, 0)` and `store64(Y.tx_head, 0)`: X's counters start over.
3. `store64(Y.hello_commit, epoch_X)`: commits steps 1 and 2.
4. `notify()`.

**Accepting** the peer (X sees a committed hello with an epoch it has not accepted yet):

1. Check `magic`, `version` and that the peer's ring fits the remote region.
   On failure: `Incompatible`.
2. Reset local state for the new session: `consumed = 0`, tx `tail = 0`.
3. Publish hello again. Y may have a fresh region that never saw X's earlier
   announcement (for example after Y re-created it), and this also zeroes X's
   counters in Y's region for the new session.
4. `store64(Y.ack, epoch_Y)`, `notify()`.

X is **Established** when it has accepted `epoch_Y` and its local `ack` equals
`epoch_X`. Only then does X send data or read its rx ring.

Why this is safe:

- Y zeroes X's `rx_tail` before it commits its epoch (store order), so X never
  parses a stale tail from an earlier session.
- Y sends only after X acknowledges `epoch_Y`, and X acknowledges only after it
  has reset its counters.
- A restarted endpoint announces a new epoch; the peer resets on seeing it.

**Delivery**: within a session every message is delivered once, in order, with
none lost. Messages in flight when a session ends are dropped. There is no
resume across sessions.

`restart()` starts a new session with a new epoch. Use it after the lower layer
reconnects, or to recover from `Broken`.

## 6. States

| State | Meaning | send | drain |
|---|---|---|---|
| `Handshaking` | Hello published, waiting for the peer | `false` | Runs the handshake |
| `Established` | Session up | Sends | Delivers |
| `Broken` | Lower layer failure or corrupt ring | `false` | Waits for a new peer epoch |
| `Incompatible` | Peer has another version or an invalid geometry | `false` | Waits for a new peer epoch |

The handshake advances inside `poll()`, which `drain()` also calls. Nothing
blocks: the application waits with the lower layer's own mechanism and calls
`poll()`/`drain()` when woken or on a timeout.

## 7. Threads

One thread may call `send` while another calls `poll`, `drain`, `receive`,
`next_size` and `restart` (the receiving side). With a single thread, all of
them may be called from it, and `send` also from inside a `drain` callback.

The receiving side detects session changes. Before it resets the session it
moves the state away from `Established` and waits until no `send` is running,
so a `send` either completes in the old session or returns `false`.

## 8. Counters

`tx_messages`, `tx_bytes`, `tx_full`, `tx_too_large`, `tx_errors`,
`rx_messages`, `rx_bytes`, `rx_crc_errors`, `rx_protocol_errors`, `sessions`,
`resets`.

## 9. API

See `include/pairring/`: `region.hpp` (the lower layer contract),
`config.hpp` (geometry and layout), `channel.hpp` (the channel),
`mapped_region.hpp` (a ready `RemoteRegion` for memory mapped into the process)
and `crc32c.hpp`.

The channel allocates nothing, not even at initialization: the ring lives in
the local region that the lower layer provides.

## 10. Not in version 1

Several producers on one ring, broadcast, priorities, doorbell suppression,
resuming a session, security.
