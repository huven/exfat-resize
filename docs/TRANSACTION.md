# Resize transaction

The library grows exFAT in place. It is intentionally not a journaled or
automatically resumable operation. The caller must keep the filesystem
unmounted, prevent concurrent access, and make a verified backup first.

The complete directory tree, allocation references, boot regions, and geometry
are validated before the first write. During preflight, the library rebuilds
the target allocation model from directory and system-file metadata, validates
FAT-described chains and bad-cluster markers, and reconciles ownership with the
source allocation bitmap. Shared clusters, referenced-but-free clusters, and
allocated clusters with no recognized owner are rejected. FAT entries
belonging to `NoFatChain` allocations are ignored, as required by exFAT. The
replacement allocation bitmap is placed entirely in newly added clusters. If
validation fails, the device is not modified.

## Transaction sequence

| API stage | Step | Writes issued | Boot geometry | Recovery after failure |
|---|---|---|---|---|
| `PREFLIGHT` | Preflight and allocation-model construction | None | Old | No write was attempted; correct the reported error and retry when appropriate |
| `PREPARING` | Begin transaction | Main `VolumeDirty` set and synchronized | Old | Source layout remains authoritative; run a checker before retrying |
| `PREPARING` | Relocate heap prefix | Allocated displaced clusters copied to non-authoritative target locations | Old | Source layout remains authoritative; run a checker before retrying |
| `RESIZING` | Write FAT | Rebuilt target FAT written from the validated allocation model | Old | Restore the verified backup; old FAT is no longer authoritative |
| `RESIZING` | Write bitmap | Replacement bitmap written in newly added tail clusters from the same model | Old | Restore the verified backup |
| `RESIZING` | Rewrite directories | Cluster references and affected entry-set checksums updated | Old | Restore the verified backup |
| `RESIZING` | Commit backup boot region | Backup geometry and checksum describe the target | Main old, backup new | Restore the verified backup |
| `RESIZING` | Commit main boot region | Both boot regions describe the target | New | Restore the verified backup |
| `FINALIZING` | Complete transaction | Attempt to clear and synchronize main `VolumeDirty` | New | Target is complete; run a checker and do not retry the resize |
| `COMPLETED` | Return success | None | New | Clean target is ready for use |

The dirty flag is synchronized before any other destructive write. Metadata
and relocated data are synchronized before either boot region is changed. The
backup region is committed before the main region, and the main dirty flag is
cleared only after both regions and their checksums have been synchronized.

`exfat_resize()` reports the recovery boundary through its optional stage
output. It selects the reported stage before attempting the first write of a
phase, so the value is conservative when that write reports failure without
taking effect. The library publishes the selected stage before returning and
does not promise automatic resume or rollback.

A `PREPARING` failure may leave `VolumeDirty` set and may leave relocated data
in locations which are not authoritative under the source geometry. The source
layout itself remains intact. A `RESIZING` failure may leave a mixture of old
and target metadata and requires the verified backup. A `FINALIZING` failure
occurs only after all target metadata and both target boot regions have been
synchronized; only the final dirty-state update is uncertain.

If execution terminates before the caller receives a stage, the caller cannot
determine which transaction boundary was reached. It must conservatively
assume `RESIZING`, must not retry the resize, and should restore the verified
backup.

The rows between synchronization points describe execution order, not separate
durability guarantees. A power loss may persist an arbitrary subset of writes
issued since the preceding synchronization.

## Memory requirements

The library requests a 1 MiB I/O work buffer through the caller's allocator.
It uses that buffer for boot-region I/O, cluster relocation, batched FAT and
bitmap output, and buffering directory entry sets.

During preflight, it requests a snapshot of the used portion of the source FAT,
rounded up to a filesystem sector. The snapshot is filled by one block-device
read, provides all source FAT lookups, and is released before the first write.
Its size is approximately four bytes per source cluster.

It separately requests a cache block containing three filesystem sectors.
Dedicated caches retain source directory data, source bitmap data, and target
directory data. Directory entry sets are checksummed and rewritten one 32-byte
entry at a time.

In addition, `exfat_resize()` requests a writable allocation model containing
one 32-bit value per target cluster. The model distinguishes free clusters,
bad clusters, `NoFatChain` allocations, and target FAT links. It is completely
built and checked against the source bitmap before the dirty flag is set, then
used to generate the target FAT and bitmap and to traverse target directories.
The caller controls how allocator-backed working memory is backed; anonymous
memory and a memory-mapped temporary file are both possible.

Directory traversal uses an additional allocator-backed worklist. It may grow
during preflight, but its final capacity is retained and reused during the
rewrite, so traversal cannot encounter a new allocation failure after the
volume has been marked dirty.

A bad-cluster marker remains valid only while it describes the same physical
sectors. Preflight rejects a target whose enlarged FAT would consume a source
cluster recorded as bad; bad clusters that remain at the same physical heap
location are preserved.
