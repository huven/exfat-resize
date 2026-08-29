# C library reference

The `exfat-resize` C11 library grows an existing exFAT filesystem in place. It
accepts caller-provided block-device and allocation interfaces, has no operating
system dependency, and reports the recovery boundary reached by every call.

Before integrating the library, read the operational [safety
requirements](../README.md#safety), [filesystem compatibility
requirements](../README.md#filesystem-compatibility), and [transaction and
recovery guarantees](TRANSACTION.md). The library is not a filesystem checker,
partitioning library, journal, rollback mechanism, or repair facility. The
caller must provide exclusive access to the filesystem and a verified backup.

The public interface is declared in `<exfat_resize.h>`. Headers under `lib/`
are private implementation details.

## Calling `exfat_resize`

```c
enum exfat_resize_error exfat_resize(
    const struct exfat_resize_block_device *device,
    uint64_t target_size,
    const struct exfat_resize_allocator *allocator,
    const struct exfat_resize_monitor *monitor,
    enum exfat_resize_stage *stage);
```

`device`, `target_size`, and `allocator` are required. `monitor` and `stage`
may be null. The library validates the required argument objects before
performing I/O, allocating memory, or invoking a monitor callback.

The objects supplied through nonnull pointers, their callback pointers and
contexts, and callback-owned state must remain valid and unchanged until the
call returns. The library retains none of them after return. A nonnull `stage`
must remain writable until return.

`target_size` is the requested filesystem size in bytes. It is rounded down to
a whole filesystem sector. The rounded target must be larger than the current
filesystem, fit in the device view, and satisfy the geometry and compatibility
requirements. Shrinking is not supported. The library does not enlarge the
backing storage or interpret a partition table.

Sector zero in the supplied device view must be the main exFAT boot sector.
Callback sector numbers are zero-based within that view. The library does not
add the boot sector's `PartitionOffset` to callback addresses.

A minimal call using ordinary heap allocation is:

```c
#include <exfat_resize.h>

#include <stdlib.h>

static void *allocate(void *context, size_t size)
{
    (void)context;
    return malloc(size);
}

static void deallocate(void *context, void *memory, size_t size)
{
    (void)context;
    (void)size;
    free(memory);
}

struct exfat_resize_allocator allocator = {
    .allocate = allocate,
    .deallocate = deallocate
};
enum exfat_resize_stage stage;
enum exfat_resize_error error;

error = exfat_resize(&device, target_size, &allocator, NULL, &stage);
```

The example omits construction and exclusive ownership of `device`, which are
application-specific. Always interpret an error together with `stage` before
deciding how to recover.

## Block-device contract

`struct exfat_resize_block_device` describes a synchronous, sector-addressed
device:

- `sector_size` is a power of two from 512 through 4096 bytes;
- `sector_count` is nonzero;
- `read`, `write`, and `sync` are all required;
- `context` is passed unchanged to each callback.

The filesystem sector size is read from the exFAT boot sector and must be a
multiple of the device sector size. The library adapts filesystem-sector I/O
to the smaller callback sectors when necessary.

`read` and `write` are called only with a nonzero, in-range transfer. The
buffer contains at least `sector_count * sector_size` bytes, and the callback
must access only those bytes. A callback returns zero only after completing
the entire transfer; any nonzero result reports an I/O failure. A failed write
may already have modified some or all of its range. A successful write must be
visible to later reads through the same device view.

`sync` returns zero only after every preceding successful write is durable.
The library's recovery guarantees depend on the operating system and storage
device honoring that contract.

Callbacks are synchronous. Buffers are borrowed only for the callback and must
not be retained or accessed after it returns.

## Allocator contract

`struct exfat_resize_allocator` is a required resource provider with its own
context. Both `allocate` and `deallocate` are required; the library has no
default allocator.

`allocate` is called only with a nonzero size. It returns either null or at
least that many readable and writable bytes, aligned for `max_align_t`, with
the effective-type behavior of storage returned by C11 `malloc`. Every live
allocation must be disjoint from other live allocations, public argument
objects, and callback-owned state.

Every successful allocation is passed exactly once to `deallocate`, using its
original pointer and size, before `exfat_resize` returns. `deallocate` is never
called with a null pointer or zero size. A null allocation result produces
`EXFAT_RESIZE_OUT_OF_MEMORY`.

Working-memory sizes and lifetimes are documented under [Memory
requirements](TRANSACTION.md#memory-requirements). Applications may use heap
memory, memory-mapped temporary storage, or another implementation satisfying
the allocator contract.

## Operation monitor

`struct exfat_resize_monitor` is optional and has a context independent from
the allocator and device contexts. In a nonnull monitor,
`cancellation_requested` and `report_event` are independently optional.

Both callbacks run synchronously on the thread executing `exfat_resize`. They
must be quick, nonblocking, safe to call repeatedly, and must not reenter the
active resize. They are responsible for safely accessing any state shared with
another thread, signal handler, console-control handler, or event loop. A C++
exception must not escape across the C interface.

### Cooperative cancellation

`cancellation_requested` returns zero to continue and nonzero to request
cancellation. The library latches the first nonzero result, so the callback
does not need to keep returning nonzero.

Cancellation is cooperative. It is observed only at safe transaction
boundaries and cannot preempt a block-device or allocator callback already in
progress. Cleanup and synchronization already required for an issued write are
allowed to finish. When a request is observed, `exfat_resize` returns
`EXFAT_RESIZE_CANCELLED`, and `stage` describes the applicable recovery path.

Invalid public arguments are rejected before the cancellation callback is
consulted. A concrete allocation, read, write, or synchronization failure is
not replaced by a cancellation request arising during that operation. Once
the clean target has been synchronized and `COMPLETED` has been reported, the
operation returns success even if that final event causes cancellation to be
requested.

Cooperative cancellation does not protect against process termination, a
crash, power loss, `SIGKILL`, or `TerminateProcess`. If the caller does not
receive a recovery stage, follow the conservative abnormal-termination
procedure in README.

### Structured events

`report_event` receives a borrowed `struct exfat_resize_event`. The pointer is
valid only during the callback. Reporting is observational: the callback has
no result and cannot replace the library result or directly alter control flow.
It may update caller-owned state that a later cancellation callback observes.

Every event contains:

- `stage`: the authoritative recovery stage at delivery time;
- `level`: a generic filtering and presentation severity;
- `code`: a permanent semantic identifier;
- `value`: a code-specific unsigned integer, or zero when unused.

Event levels form the closed `enum exfat_resize_event_level`. Event codes use
an open `uint32_t` namespace so later 2.x releases can add codes without
changing the monitor structure. Callers must tolerate unknown event codes by
ignoring them or displaying their numeric stage, level, code, and value.
Published codes are never renumbered or reused with different semantics.

The initial code is `EXFAT_RESIZE_EVENT_CODE_STAGE_ENTERED`. It is emitted at
`EXFAT_RESIZE_EVENT_LEVEL_INFO` after each stage becomes authoritative. A
successful call reports these stages once and in order:

1. `PREFLIGHT`
2. `PREPARING`
3. `RESIZING`
4. `FINALIZING`
5. `COMPLETED`

The event value is zero for the first four stages. At `COMPLETED`, it is the
exact resulting filesystem size in bytes after filesystem-sector rounding.
Calls rejected by the initial structural argument validation emit no events.
Target and filesystem validation that requires device I/O occurs after
`PREFLIGHT` is reported. Failures emit only the stages reached before return.

## Results and recovery stages

The returned `enum exfat_resize_error` describes why execution stopped.
`EXFAT_RESIZE_SUCCESS` means the resized clean target was synchronized.
`EXFAT_RESIZE_CANCELLED` means a cooperative request was observed. Other
values distinguish invalid arguments or filesystem state, unsupported
features, bounds and arithmetic failures, allocation failure, I/O failure, and
an internal invariant failure. The public header provides the concise meaning
of every result constant.

When `stage` is nonnull, the library always stores the recovery boundary
reached, including for invalid arguments and failures:

| Stage | Durable meaning | Recovery after failure |
| --- | --- | --- |
| `PREFLIGHT` | No filesystem write was attempted | Correct the reported problem and retry when appropriate |
| `PREPARING` | The source layout remains authoritative | Run a filesystem checker before retrying |
| `RESIZING` | Authoritative source metadata may have been overwritten | Restore the verified backup |
| `FINALIZING` | The complete target was synchronized; final dirty state is uncertain | Run a filesystem checker and do not retry the resize |
| `COMPLETED` | The resized clean target was synchronized | Use the resulting filesystem |

The stage is deliberately conservative when a write reports failure: the
library selects the new stage before attempting the first write carrying that
stage's risk. See [Resize transaction](TRANSACTION.md) for exact write and
synchronization ordering.

## Concurrency and callback lifetime

The library does not invoke callbacks concurrently within one public call and
performs no synchronization between calls. Concurrent calls must use different
backing devices. Shared callback state must support the caller's chosen
concurrency and the monitor's external request source.

A callback must not call back into the active resize. Callback functions and
contexts remain caller-owned and must stay valid until return. The library
retains no device, allocator, monitor, stage, event, buffer, or context pointer
after its documented lifetime.

## Building and linking

Downstream CMake projects can include the source tree with `add_subdirectory()`
or consume an installed package:

```cmake
find_package(exfat_resize 2.0 CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE exfat_resize::exfat_resize)
```

The project distributes a static library. Build consumers with the header and
library from the same release and with ABI-compatible compiler settings for the
target platform. Separately compiled artifacts from different releases or
deliberately different compiler ABIs are not promised to interoperate.

## Compatibility and migration from 1.x

The public C API remains source compatible throughout a major release series.
Incompatible source changes require another major release. New event codes may
be added during 2.x because callers are required to tolerate unknown codes.

Version 2.0 deliberately removes `struct exfat_resize_options`. In 1.x, the
allocator was nested in that structure and `exfat_resize` took four arguments:

```c
struct exfat_resize_options options = {
    .allocator = allocator
};

error = exfat_resize(&device, target_size, &options, &stage);
```

In 2.x, pass the required allocator and optional monitor as separate primary
arguments:

```c
struct exfat_resize_monitor monitor = {
    .context = monitor_context,
    .cancellation_requested = cancellation_requested,
    .report_event = report_event
};

error = exfat_resize(&device, target_size, &allocator, &monitor, &stage);
```

Pass null for `monitor` when neither callback is needed. The function keeps the
same symbol name, but the removed options tag and the five-argument declaration
make old source fail explicitly at compile time. Recompile callers against the
matching 2.x header and static library; do not reuse a 1.x object file.
