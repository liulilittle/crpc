# Inter-process communication

[简体中文](ipc.zh-CN.md) | **English**

This page covers the three primitives for moving data between processes on one
machine: named shared memory, the byte ring, and the value queue, plus the
full-duplex channel built from them. All three live in `crpc::ipc` except the mapping
itself, which is `crpc::fs::shared_memory`.

The library is asynchronous at the bottom and offers synchronous forms on top. That
shapes every API here, so the ordering rules are stated first.

## Blocking rules

| Layer | What it does |
|---|---|
| `try_send` / `try_receive` (byte ring, queue) | Never waits, on any thread. These are the primitives. |
| `send` / `receive` with a timeout (channel) | On a shard thread inside a coroutine: `runtime::park()`, resumed by `runtime::notify()`. On a plain thread: a bounded wait with the given timeout. |
| A zero timeout | Means "do not wait": both return the non-blocking result. |

The rule that matters: **a synchronous form must never block a shard thread**. Blocking
there stops every other task on the shard, and a peer that is itself waiting on that
shard then deadlocks. The channel implements this by parking; the test
`ipc_async_shard_thread_parks_without_blocking_the_shard` runs the producer and the
consumer as two tasks on one shard and fails if the wait blocked instead of parking.

## Named shared memory — `fs::shared_memory`

```cpp
#include <crpc/fs/shared_memory.hpp>

// Creator:
auto owner = crpc::fs::shared_memory::open("metrics.v1", 4096,
                                           crpc::fs::shared_memory_mode::create);
// Peer:
auto peer = crpc::fs::shared_memory::open("metrics.v1", 0,
                                          crpc::fs::shared_memory_mode::open_existing);
peer.value().bytes()[0] = std::byte{42};
// `owner` now sees 42 through its own `bytes()`.
```

`bytes()` starts after a 16-byte header this class writes, so `size()` is the length
the creator asked for on both platforms. That header exists because Windows rounds a
mapping up to the page size and has no supported way to recover the requested length
afterwards, while POSIX reports the `ftruncate` size exactly; without it one platform
would hand back a page-rounded number.

`remove_unlinks_the_name()` reports a platform difference rather than hiding it:

| | POSIX | Windows |
|---|---|---|
| `remove(name)` | `shm_unlink`; a later `open` fails with `not_found` | Reports whether the name exists; the object lives while any handle is open |
| `remove_unlinks_the_name()` | `true` | `false` |
| Existing views after removal | Stay valid | Stay valid |

Code that relies on "nobody can attach after I remove it" must check the flag.

Name rules are the intersection of both namespaces, so a name that passes is portable:
1–96 bytes, letters/digits/`.`, `_`, `-`, no leading dot, no separator.

## Byte ring — `ipc::ring_buffer`

A single-producer single-consumer ring of bytes, lock-free, laid out as integers only
so it can live inside a mapping.

```cpp
crpc::ipc::ring_control control{};
crpc::ipc::ring_view::initialise(&control, capacity);
auto producer = crpc::ipc::ring_view::attach_producer(&control, payload);
auto consumer = crpc::ipc::ring_view::attach_consumer(&control, payload);

producer.try_write(frame);              // false when full; nothing written
const std::size_t n = consumer.try_read(buffer);  // 0 when empty
```

Two design points that are not obvious:

- **No sentinel slot.** A full ring holds exactly `capacity()` bytes. Occupancy is
  `write - read` over two free-running 64-bit counters, each written by one side.
- **Ordering.** The producer stores the write index with `release` after the bytes;
  the consumer loads it with `acquire` before them. The opposite directions are
  relaxed on purpose: a stale read index only makes the producer think the ring is
  fuller than it is, which costs throughput and cannot corrupt data.

A `ring_view` is either producer or consumer, never both, so a ring cannot acquire a
second producer by accident.

## Value queue — `ipc::spsc_queue`

The same ring, of values instead of bytes.

```cpp
crpc::ipc::spsc_queue<std::uint64_t> q(1024);
q.push(7);            // false when full
std::uint64_t out;
q.pop(out);           // false when empty; `out` untouched
```

`T` must be trivially copyable and default-constructible, enforced with
`static_assert` — a value owning a pointer is meaningless in a mapping the peer also
sees. The control block records the element size, so mapping a queue of `int` as a
queue of `double` is refused rather than read as plausible garbage.

For a cross-process queue, use the view form with a mapping; `spsc_queue` owns its
storage and is for per-shard work queues inside one process.

## Full-duplex channel — `ipc::channel`

Two rings in one mapping: the creator produces into one and consumes from the other,
and the joiner does the reverse. That is why a channel is not "one ring with a lock" —
a shared ring would have two producers.

```cpp
auto creator = crpc::ipc::channel::create("worker.1", 64 * 1024);
auto joiner  = crpc::ipc::channel::join("worker.1");

creator.value().try_send(frame);        // creator -> joiner
std::byte buf[256];
joiner.value().try_receive(buf);        // reads what the creator sent
```

The mapping layout, all at fixed offsets from its base:

```
channel_header    magic, capacity, readiness
ring_control      outbound (creator -> joiner)
ring_control      inbound  (joiner -> creator)
payload           outbound, `capacity` bytes
payload           inbound,  `capacity` bytes
```

Readiness matters: creating a channel is several stores, and a joiner that attaches
mid-way would see a half-initialised region. The creator publishes `ready` last with
release ordering; a joiner that acquire-loads it and finds it unset gets
`unavailable` — "still being created, retry" — rather than a silently wrong view.

## Cross-process wake

A process cannot observe the other's memory, so there is no signal from the writer to
the reader across processes. `try_send` rings the runtime bound with `bind_runtime()`,
which wakes a reader parked on the *same* runtime; across processes the reader's own
event loop has to poll, and `poll_ready()` exists for exactly that.

## Measured cost

Release build, 8-byte frames. The Windows and Linux rows differ and both are shown:
the same code measures differently on the two toolchains, so a single number would be
a claim about one machine presented as a claim about the library.

| Operation | Cost |
|---|---|
| `spsc_queue` push + pop, one thread | 4.0 ns/op |
| `spsc_queue` push + pop, two threads | 9.3–13 ns/op (scheduler noise) |
| `ring_buffer`, 64-byte frame | 9.0–10.9 ns/op |
| `ring_buffer`, 1 MiB frame | 42–56 µs (≈40 GB/s, memory bandwidth) |
| Ring index, power-of-two capacity | 3.0–4.5 ns/op |
| Ring index, non-power-of-two capacity | 3.5–6.4 ns/op |
| **`std::mutex` + `std::deque`, same pattern** | **15.6 ns/op** |

The mutex row is the reason the lock-free form exists, but the size of the win is
toolchain-dependent and worth stating precisely: on Windows the mutex path is 15.6
ns/op against 4.0 (3.9x), while the same code on Linux measures 9.8 against 4.0
(2.4x). Both are the *uncontended* case. The direction is the same on both; quoting
only the Windows ratio would overstate what a Linux build gets.

The index reduction is a mask when the capacity is a power of two and a remainder
otherwise — about 30% on the index alone. A single-element queue operation is a store
plus two atomic indices, so SIMD has nothing to widen there; a 1 MiB frame is already
at memory bandwidth through `memcpy`, which uses the widest load/store available.

### One optimisation that was measured and rejected

`net::connection::read_exact` drains its inbound buffer one byte at a time
(`front()`/`pop_front()` in a loop). Replacing that loop with a chunked
`insert(end, begin, end)` + `clear()` looks obviously cheaper — one call instead of N.
Measured on a 64 KiB deque, drained into a pre-reserved vector:

| Drain | Cost per 64 KiB |
|---|---|
| per-byte `front()`/`pop_front()` loop | 295–315 µs |
| chunked `insert` + `clear` | 403–405 µs |

The chunked form is about 35% **slower**. `std::deque` stores its elements in segments,
so a range `insert` has to walk segment headers rather than a flat buffer, while the
per-byte loop touches one segment header per several bytes and predicts well. The
number is recorded here because "fewer calls must be faster" is a plausible-sounding
rule that this container does not obey; the code was left as it was.

### Where this queue is *not* used, and why

`runtime::submit` and `push::hub` both queue from more than one thread: a server
connection submits from whichever thread owns that connection, and the hub is fed by
every publisher. Replacing those with an SPSC queue would be a correctness bug, not an
optimisation — two producers would corrupt the indices that the whole design rests on.
The measured 3.9x is therefore a reason to *reach for* this queue in a single-producer
path, not a reason to force it where the producer is not single.

## Testing

Each primitive has a test that would fail for the reason the primitive exists, not just
for a wrong value:

- shared memory: writes are read back through a **second** handle, because a private
  copy of the pages would also pass a read-back through the same handle;
- byte ring: every frame size from 1 to capacity through a 9-byte buffer, so almost
  every size straddles the wrap;
- value queue: exact capacity with no lost slot, FIFO across the wrap, and a consumer
  view that cannot push;
- channel: each direction independently, and a move that must not swap the ends;
- async: a shard-thread wait that must park rather than block.

Two cases cross a boundary rather than a branch:

- shared memory reads back through a **second process**: the test binary re-launches
  itself as a peer, so a process-private mapping would fail instead of passing;
- both platforms run the suite. On Linux (Ubuntu 24.04, g++ 13.3, the CI flags) the
  POSIX `shm_open`/`ftruncate`/`shm_unlink` branch is compiled and exercised, and its
  case reports 46 checks against 43 on Windows — the extra three are the
  `remove_unlinks_the_name() == true` path, which only POSIX takes.

Still not covered: the memory-ordering argument needs a weakly-ordered CPU to be
exercised, and a real reader's event-loop wake through `poll_ready()` has no external
poller to attach to in a test, so the tests poll `try_receive` instead.
