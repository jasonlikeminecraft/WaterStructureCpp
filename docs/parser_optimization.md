# Parser Optimization Log

This document tracks parser performance work and reusable techniques. Results use
Release builds (`-O3 -DNDEBUG`) and are recorded separately for parsing and for
end-to-end conversion.

## Acceptance

- Streaming is a library-wide compatibility requirement: memory use must be
  bounded by buffers and chunk batches rather than the decoded structure volume.
- A runnable reader is complete when its `parse_ms` median is below 25 seconds.
- Each benchmark is repeated at least three times; correctness is checked with
  dimensions, non-air counts, entities, and a chunk checksum where applicable.
- Peak private memory for the large sample must remain below 500 MiB.
- Large derived files are generated in a temporary directory and are not checked
  into the repository.
- Formats without an implemented reader, such as SIBI, are recorded as unsupported
  rather than treated as performance failures.

## Baseline Samples

| Sample | Size | Volume | Notes |
| --- | ---: | ---: | --- |
| `tests/fixtures/private_formats/麦迪维尔等候大厅.schematic` | 519 x 256 x 519 | 68,956,416 | Real large sample; 22,492,449 non-air blocks |

The large Schematic is converted to writer-supported formats for additional large
reader and round-trip coverage. Small private fixtures remain the correctness
baseline for formats without a large native sample.

## Methods

### OPT-010: BDX decode window and bounded save batches

BDX commands are cursor-dependent and must be decoded serially. The hot path now
reads Brotli output through a heap-backed 1 MiB window, so primitive fields do not
invoke `istream::read` separately. World import retains a bounded chunk cache and
submits evicted chunks in groups of 16, reducing LevelDB write transactions without
changing chunk order or memory guarantees. `WATER_STRUCTURE_PROFILE=1` reports the
number and duration of save batches and any chunk reloads.

On the Kuudra validation sample, the current path used 9 save batches, 0 reloads,
and 42 ms in world saves; the remaining time is BDX decode and block-state work.

### OPT-001: Range-limited Schematic chunk materialization

**Status:** implemented and verified on the large real sample.

The old Schematic reader scanned the complete `Blocks` and `Data` arrays for every
requested chunk batch. The optimized path derives the source X/Z range for each
requested chunk, walks only that range, and resolves each legacy `(block_id, data)`
pair once per batch. `get_chunks_layer0()` also skips initialization of layer1,
which Schem-like writers do not consume.

This method is reusable for dense formats whose source storage is already laid out
by Y/Z/X and whose consumers request bounded chunk batches.

Before optimization, the large sample's Schematic-to-Schem conversion spent about
37.6 seconds in `get_chunks` and 39.5 seconds end to end. Three post-change
benchmark runs produced `get_chunks_ms` values of 1.72s, 1.70s, and 1.55s;
the median parse time was 1.90s and the median total benchmark time was 5.35s.
The profiled Schematic-to-Schem conversion completed in 2.35s. Peak working set
was about 398 MiB and the checksum remained `12819008811232`.

The output was opened again as SchemV1 and retained size `519x256x519` and
`22,492,449` non-air blocks. After moving the decoded NBT arrays into the reader,
three direct Release runs reported `parse_ms` = 470ms, 440ms, and 462ms, with
peak private memory about 276 MiB.

### OPT-002: Reuse range-limited materialization for dense palette formats

**Status:** implemented and verified for Litematic and MCStructure on the large
derived samples.

Litematic and MCStructure use dense palette/index arrays with different source
index orders. Their readers now derive the source X/Z intersection for each
requested chunk and visit only those coordinates. The shared layer0 entry point
also avoids allocating or clearing the secondary block layer when a writer only
needs primary blocks. This keeps the interface compatible while making the
optimization reusable across formats with bounded chunk requests.

Litematic was benchmarked three times in Release on the large derived sample
through the streaming visitor path: `parse_ms` = 0.514s, 0.423s, 0.427s
(median 0.427s); `get_chunks_ms` = 0.544s, 0.521s, 0.559s (median 0.544s).
All runs reported size `519x256x519`, 1,089 chunks, zero entities, checksum
`12819008748065`, and 22,492,449 non-air blocks.

The generated MCStructure is 527 MiB. Its reader now uses the streaming NBT
method recorded in OPT-006 below; the large two-layer benchmark completed in
three final Release runs with parse times of 4.179s, 4.029s, and 3.909s
(median 4.029s), checksum `12819008811232`, and peak private memory 407.641 MiB.

### OPT-003: Indexed range lookup in SparseBlockStore

**Status:** implemented for all readers backed by `SparseBlockStore`.

The shared store is used by AxiomBP, BCF, Construction, CovStructure, all
FuHong, GangBan and MianYang variants, BDS/NexusNP, QingXu, and TimeBuilder.
Its old materializer walked every stored block for every requested chunk batch.
The new implementation uses the map's `(x, y, z)` ordering to seek directly to
each requested source X/Y row and visits only the requested Z interval. The
layer0-only entry point is exposed by each wrapper, so Schem-like writers do not
initialize the secondary layer unnecessarily.

As a large synthetic validation, a 1,048,576-block FuHongV2 sample
(`128x64x128`) was benchmarked three times in Release. `parse_ms` was 2.465s,
2.505s, and 2.396s (median 2.465s); `get_chunks_ms` was 68.7ms, 105.3ms, and
105.8ms. All runs reported 64 chunks, zero entities, checksum
`1113859292992`, and the inspect pass reported 1,048,576 non-air blocks.
The remaining readers inherit the same materialization implementation and are
tracked individually in the status table as native fixtures become available.

### OPT-004: Lazy chunk index for vector-backed readers and layer0 writers

**Status:** implemented and verified by Release rebuild plus core tests.

BDX, KBDX, MCFunction, RunAway, and IBImport keep decoded blocks in vectors. Their
old materializers revisited the complete vector for every requested chunk batch.
`ChunkBlockIndex` now lazily groups vector indices by world chunk and invalidates
the grouping when an offset changes. Requested batches visit only their buckets,
while preserving the source vector order within each bucket. This is useful for
readers whose parse phase already produces a flat block list.

All writers that serialize only the primary layer (AxiomBP, BDX, FuHong, IBImport,
Litematic, SchemV1/V2, and Schematic) now call `get_chunks_layer0()`. MCStructure
remains on `get_chunks()` because it intentionally writes both layers.

The synthetic BDX fixture was run three times after the index change: `parse_ms` =
0.265ms, 0.239ms, 0.285ms (median 0.265ms), `get_chunks_ms` = 0.0097ms,
0.0090ms, 0.0092ms. Dimensions were `8x9x10`, three entities were retained, and
the checksum was `4392299447`.

### OPT-005: Streaming chunk and entity visitors

**Status:** implemented in the public interface and world conversion path.

`IStructure::visit_chunks()` and `visit_chunk_nbt()` now process one requested
chunk at a time by default. `visit_chunks()` deliberately uses the full
`get_chunks()` contract, including layer1, while layer0-only writers call
`get_chunks_layer0()` explicitly. This keeps compatibility with every existing
reader while bounding temporary materialization to a single chunk.
`convert_to_world` uses both visitors, so parser-to-world conversion no longer
retains a complete batch `ChunkMap` or `NbtChunkMap`. Specialized readers can
override the visitors later without changing writers or world conversion.

The current large-sample Release measurements remain below the new memory gate:
Schematic streamed three times with `parse_ms` = 0.510s, 0.501s, 0.500s
(median 0.501s), `get_chunks_ms` = 0.817s, 0.788s, 0.783s, checksum
`12819008811232`, and peak private memory about 276 MiB. Litematic streamed
three times with peak private memory about 276 MiB. MCStructure now uses the
same visitor contract while retaining both layers.

### OPT-006: Streaming MCStructure NBT and compact index storage

**Status:** implemented and verified on the 527 MiB derived sample.

The previous MCStructure reader first built the complete libnbt object tree and
then copied two full `int32` index arrays. The new reader walks the root NBT
compound with `stream_reader`, skips unrelated payloads, reads `block_indices`
one integer at a time, and only builds a tree for the small palette metadata.
Index arrays use 16-bit storage when palette indices fit, with an automatic full
width fallback for unusual palettes. This removes the multi-hundred-megabyte
temporary NBT tree and keeps the large two-layer reader below the 500 MiB gate.

### OPT-007: Streaming Schem BlockData with sparse row checkpoints

**Status:** implemented and verified on the newly uploaded 41 MiB Flight sample.

SchemV1/V2 no longer materialize the complete NBT tree, byte array, or packed
block-index vector. The gzip payload is copied to a temporary file while row and
256-block checkpoint offsets are recorded. Chunk reads seek to the nearest valid
checkpoint, read only the remaining encoded row bytes, and decode varints directly
from a reusable heap buffer. A checkpoint that cannot fit in 16 bits is marked as
missing and safely falls back to the row start. This avoids the former multi-GB
fixed-width `.decoded` file and preserves unusual wide-row inputs.

The sample is `2610x282x2615` (26,896 chunks, 510,146,162 non-air blocks).
On the current Windows Release build, the pulled fixed-width implementation
reported `parse_ms` = 24.064s, `get_chunks_ms` = 57.003s, and total = 83.982s.
The sparse-checkpoint run reported `parse_ms` = 18.692s,
`get_chunks_ms` = 20.323s, and total = 39.989s. Checksum remained
`393687046530647`; peak private usage was 160.949 MiB. A small 136x70x135
fixture improved from 45.5ms to 18.4ms for chunk traversal. Large I/O buffers
are heap-backed so the reader also runs under the default Windows stack reserve.

### OPT-008: Batch Bedrock subchunk writes and omit empty layer1

**Status:** implemented and verified on Schematic and the Flight Schem sample.

`BedrockWorldAdapter` now collects all subchunks of a chunk into the existing
`BedrockWorldOperator::World::saveSubChunksBatch()` call. This replaces a
metadata write plus a data write for every subchunk with one LevelDB batch per
chunk. The adapter also checks whether layer1 contains a non-air runtime ID;
the normally empty secondary layer is no longer transposed, allocated in BWO,
or passed to its encoder. Formats with a populated secondary layer retain it.

On Flight, the former fixed-index world stage was 49.017s. With this method
it was 35.734s: 10.199s in save batches and 25.529s in generic chunk
materialization. The remaining materialization cost motivated OPT-009.

### OPT-009: Direct Schem 16-layer slab-to-world stream

**Status:** implemented and verified by a full source-stream checksum.

For direct Schem-to-world conversion, SchemV1/V2 with zero block offset and a
palette index range no wider than 16 bits bypass generic `get_chunks()` row
materialization. The reader retains one 16-Y-layer palette-index slab;
for the Flight sample this is 208.287 MiB. It materializes one Z chunk stripe
at a time and sends those subchunks through the batch world writer. Unusual
large palettes and non-zero offsets deliberately retain the generic indexed
path.

A Release CLI conversion of Flight completed in 35.231s with profiling and
verification enabled (`decode_ms` 6.580s, `materialize_ms` 5.101s,
`save_ms` 10.882s). The stream accumulated the same chunk, subchunk, layer0,
and layer1 checksum used by the benchmark: `393687046530647`. The direct path
retains a 208.287 MiB 16-layer source slab to preserve sequential reads; a
row-seek window reduced temporary memory but increased wall time to 85.111s and
was rejected. This avoids using a full MCWorld readback as the normal large-
sample validation method.

Full MCWorld reverse conversion remains a separate optimization target:
Bedrock disk payload decoding rebuilds block-state palettes for every subchunk
and should be used for sampled or small-fixture round trips, not every large
writer iteration. `McWorldStructure` now overrides `visit_chunks()` to retain
requested batches rather than falling back to one chunk per visitor call.

### OPT-010: Streaming BDX and bounded world batches

**Status:** implemented and verified with generated and real Go BDX files.

The BDX reader now parses commands directly from a 1 MiB Brotli decode window.
It no longer retains the compressed file and complete decoded command stream at
the same time. Constant-string block-state pairs, legacy pairs, and runtime-pool
entries cache their resolved runtime IDs. The decoded byte limit is expansion-
ratio bounded and supports valid streams up to 64 GiB; truncated Brotli streams
and truncated commands retain explicit error context.

The BDX writer reads at most 32 MCWorld chunks per batch and releases the source
cache immediately. Commands pass through a 256 KiB staging buffer into a Brotli
quality-6 stream, so complete chunk maps, uncompressed commands, and maximum-size
compressed output are never resident together. Command, palette discovery, and
cursor order are unchanged. Vector-backed chunk indices now use checked 32-bit
entries, halving per-block index storage during BDX-to-world conversion.

Release results on the 188x175x185 Kuudra MCWorld (2,705,661 emitted blocks):

| Operation | Before | Current | Peak private before/current |
| --- | ---: | ---: | ---: |
| MCWorld to BDX | 27.154s | 1.494s | 278.3 / 151.6 MiB |
| BDX inspect | 7.804s | 2.245s | 171.7 / 151.7 MiB |
| BDX to MCWorld | 2.959s | 1.675s | 168.6 / 159.0 MiB |

Quality 6 increased this sample from 143,935 to 173,178 compressed bytes. The
old and new writer outputs have identical canonical chunk manifests after the
input-file hash is excluded. A real Go fixture retained 308,528 blocks and 163
block entities; a clean directory-world round trip matched every chunk hash and
all entity positions and payload fields (excluding the world-added x/y/z tags).

### OPT-011: BDX bounds fast scanner

**Status:** implemented and verified on Kuudra and a time-limited Greenfield run.

The streaming world path needs the exact cursor bounds before it can translate
raw BDX coordinates into target world coordinates. The old bounds pass dispatched
every command through the full field decoder. The new pass consumes complete
fixed-width commands directly from the Brotli decode window, applying cursor
updates and bounds in batches. Variable-length commands, NBT, chests, unknown
commands, window boundaries, and truncation continue through the original checked
decoder, so malformed-input offsets and compatibility behavior are preserved.

On Greenfield, the bounds pass advanced from roughly 8.2e8 commands in 40 seconds
to 2.868e9 commands in 13.848 seconds (about 10x throughput). The process remained
streaming; the observed working set stayed near 130 MiB. The subsequent full pass
is still dominated by the approximately 1.38e9 block placements and is a separate
optimization target. Kuudra retained size `187x174x184` and `2,705,661` non-air
blocks after the fast scanner was enabled.

### OPT-012: BDX full-pass Z-run batching

**Status:** implemented and verified by core tests, Kuudra canonical hashes, and
a time-limited Greenfield run.

The dominant Greenfield command pattern alternates constant-state placement
(`command 5`) with `z++` (`command 18`). The full pass now recognizes complete
runs inside the Brotli decode window, resolves states through a small direct
cache, and sends a bounded runtime-ID span to the world writer. The writer splits
the span only at 16-block Z boundaries and fills the owning subchunk directly.
Short runs, air behavior, variable commands, NBT, and window boundaries retain
the original per-command path.

Kuudra's full pass dropped from about 300 ms to 107 ms and the complete
BDX-to-directory conversion completed in 1.253 seconds. Every canonical
chunk/subchunk runtime hash matched the previous streaming MCWorld baseline.
On Greenfield, a controlled run reached about 330 million placed blocks in
36.6 seconds after the full pass began; the previous path needed about 103
seconds to reach the same point. Working memory remained near 127 MiB and the
parser remained single-threaded, leaving chunk-partitioned workers as a later
optimization rather than a requirement for bounded memory.

### OPT-013: BDX native subchunk layout

**Status:** implemented and verified by core tests and Kuudra canonical hashes.

The generic structure API stores a subchunk in `(y,z,x)` order because that is
the established index used by readers and manifests. The BDX streaming writer,
however, receives long runs along Z and previously wrote them with a stride of
16. It also required `BedrockWorldAdapter::save_chunks` to transpose all 4096
entries of every layer before handing them to BedrockWorldOperator.

`ChunkData` now carries an internal layout flag. BDX streaming chunks use the
native `(x,y,z)` order, making Z runs contiguous and allowing the adapter to
pass the layer directly to BWO. If a bounded cache evicts a chunk, the single
reload conversion is performed before more blocks are appended. Other readers
retain the original layout and behavior.

On the Kuudra fixture, the complete streaming conversion dropped from about
154 ms to 119 ms (save time from 39 ms to 26 ms), while all 144 chunk/subchunk
canonical runtime hashes remained identical. The layout flag is in-memory only;
it does not alter manifest ordering or file compatibility.

### OPT-014: BDX layer-granular reload and asynchronous save pipeline

**Status:** implemented and measured on Greenfield and Kuudra.

The large Greenfield command stream revisits chunks after cache eviction. The
cache still owns data by chunk for cheap hashing and LRU operations, but tracks
persisted layers by `(chunkX, subY, chunkZ)`. A revisit therefore decodes only
the required subchunk instead of every Y layer in the chunk.

Chunk eviction now moves one bounded 16-chunk batch to a single background
worker. That worker performs BWO encoding and the LevelDB `WriteBatch` while the
main thread continues Brotli decoding, runtime resolution, and filling the next
batch. Only one save can be in flight. A revisit of an in-flight chunk waits for
that save before loading it, and all pending work is joined before block-entity
writes and world close, preserving database order and bounded memory.

On Greenfield, the full streaming pass improved from about 311 seconds before
the layer/native/cache work, to 190 seconds with layer-granular synchronous
saves, and then to 137 seconds with the asynchronous save pipeline. The process
remained near the low hundreds of MiB rather than materializing the 1.38 billion
blocks. Kuudra's full pass measured about 90 ms. A 128-chunk cache did not reduce
Greenfield's 621,121 layer revisits and increased the full pass to 177 seconds;
a 512-chunk experiment was slower still, so the default remains 64 chunks.
`WATER_STRUCTURE_BDX_CHUNK_CACHE` permits a bounded 64-512 override for other
input locality patterns.

### OPT-015: BDX direct decode window and Z-run routing cache

**Status:** implemented and verified by Release tests, Kuudra canonical hashes,
and a complete Greenfield conversion without manifest materialization.

The Brotli stream buffer now directly implements checked primitive reads,
skips, contiguous access, and the `std::streambuf` interface required by typed
NBT decoding. This removes the former second decoded `streambuf` layer. Input
and output windows are heap-backed and increased from 64 KiB to 1 MiB; Kuudra
therefore needs 16 Brotli decode calls per pass instead of 255, without placing
multi-megabyte arrays on the Windows stack.

The Z-run consumer no longer owns a `std::function`. It uses a non-owning
context/function pair whose lifetime is scoped to the synchronous read. The
world route keeps the previous run's raw and translated endpoint, chunk/subchunk
coordinates, and local indices. It reuses them only when the next raw run starts
at the exact previous endpoint with the same X/Y; any cursor discontinuity takes
the complete checked translation path, preserving arbitrary BDX command order.

Three Release Kuudra conversions reported full-pass times of 111 ms, 103 ms, and
108 ms (median 108 ms). Its dimensions, non-air count, sorted chunk/subchunk
runtime hashes, and canonical NBT remained byte-for-byte equal to the established
manifest after excluding input path and SHA-256 fields.

The complete Greenfield run retained bounded memory (about 250 MiB observed
working set) and reported 22.657 s for bounds plus 138.659 s for full parsing and
world filling. The asynchronous save accounting was 46.759 s of BWO encoding and
43.147 s of LevelDB writes, with only 1.911 s of foreground wait. This run does
not establish an end-to-end Greenfield speedup over the previous 131.486 s full
pass; it confirms that the remaining critical work is subchunk acquisition and
the 1.38 billion block placements, while encoding and database work remain
mostly overlapped. A templated parser experiment intended to inline the consumer
regressed Greenfield to 179.606 s and was rejected.

## MCFunction bounded parallel writer

The MCFunction writer now uses a fixed-size bounded thread pool for independent
`(chunk_z, batch_x)` encoding tasks. The producer remains single-threaded because
`McWorldStructure` owns a mutable chunk cache. Workers receive owned `ChunkMap`
values, reuse per-worker scan/state caches, and never access the source structure.
The main thread drains results in sequence order, so command ordering is identical
for every thread count.

Each in-flight result retains at most 2 MiB in memory and spills larger task output
to an isolated temporary directory. The default in-flight limit is twice the worker
count, and the temporary directory is removed through RAII on success or failure.
`ConversionOptions::thread_count` controls the library path; the CLI exposes it as
`--threads <count>`. Automatic mode selects two workers.

Command formatting uses `std::to_chars` into a 256 KiB staging buffer instead of
worker-local iostreams. Repeated disk block-state NBT payloads are resolved through
an 8 MiB bounded BWO cache; cache hits skip property-map construction and upgrade
schema evaluation. Decoded block layers are moved into BWO storage and exposed to
the world adapter as read-only spans, removing two full 4096-entry copies per layer.
On the profiled Utopia run, cumulative MCWorld source loading fell from 7.46s to
4.06s while peak private memory remained about 155 MiB.

Release measurements for the 2701x176x2701 Utopia MCWorld, with output directed to
`NUL` except for the final two-thread validation:

| Encoding workers | Wall time | CPU time | Peak private memory |
| ---: | ---: | ---: | ---: |
| 1 | 17.06s | 16.16s | 152.1 MiB |
| 2 | 9.55s | 14.94s | 154.7 MiB |
| 3 | 11.31s | 18.98s | 162.1 MiB |
| 4 | 11.05s | 18.61s | 183.6 MiB |
| 8 | 11.28s | 19.08s | 247.8 MiB |

The final two-thread real-file run completed in 10.79s. Its output is 897,044,964
bytes with SHA-256
`359696D912A4969C935CCFDEEF7A90509C7AD2A53951C22675019ED7D5BC2492`.
The 1-thread and parallel paths are also compared byte-for-byte in the core test
fixture. This CPU's useful scaling ends at two encoding workers; additional workers
increase memory bandwidth and last-level-cache contention instead of reducing wall
time.

## Format Status

| Format | Sample | Parse median | Status | Latest method |
| --- | --- | ---: | --- | --- |
| Schematic | large real sample | 0.50s | complete | OPT-001/005 |
| SchemV1 | Flight to the citadel | 8.76s | complete | OPT-007 |
| SchemV2 | large derived sample | n/a | streaming implementation shared; large run pending | OPT-007 |
| Litematic | large derived sample | 0.43s | complete | OPT-002/005 |
| MCStructure | 527 MiB derived sample | 4.03s | complete | OPT-002/005/006 |
| FuHongV2 | synthetic 128x64x128 | 2.46s | complete | OPT-003 |
| AxiomBP | synthetic fixture (1x1x1) | 0.35ms | complete on synthetic fixture | OPT-003 |
| BCF | minimal fixture (3x1x1) | 0.087ms | complete on minimal fixture | OPT-003 |
| BDS | minimal fixture (3x1x1) | 0.100ms | complete on minimal fixture | OPT-003 |
| Construction | synthetic fixture (2x1x1) | 0.21ms | complete on synthetic fixture | OPT-003 |
| CovStructure | minimal fixture (2x1x1) | 0.106ms | complete on minimal fixture | OPT-003 |
| FuHongV1/V3/V4/V5 | synthetic fixtures (2x1x1) | 0.25ms max | complete on synthetic fixtures | OPT-003 |
| GangBanV1-V7 | synthetic fixtures (1-2x1x1) | 0.24ms max | complete on synthetic fixtures | OPT-003 |
| MianYangV1/V3/V4 | synthetic fixtures (2-4x2-3x2-5) | 0.24ms max | complete on synthetic fixtures | OPT-003 |
| NexusNP | minimal fixture (2x1x1) | 0.094ms | complete on minimal fixture | OPT-003 |
| QingXuV1 | synthetic fixture (2x1x1) | 0.143ms | complete on synthetic fixture | OPT-003 |
| TimeBuilderV1 | synthetic fixture (2x1x1) | 0.105ms | complete on synthetic fixture | OPT-003 |
| BDX | synthetic fixture (8x9x10) | 0.265ms | complete on synthetic fixture | OPT-004 |
| IBImport | synthetic fixture (1x1x1) | 0.18ms | complete on synthetic fixture | OPT-004/005 |
| KBDX | synthetic fixture (1x1x1) | 0.10ms | complete on synthetic fixture | OPT-004/005 |
| MCFunction | synthetic fixture (3x1x3) | 0.10ms | complete on synthetic fixture | OPT-004/005 |
| RunAway | synthetic fixture (2x1x1) | 0.11ms | complete on synthetic fixture | OPT-004/005 |
| TIBI | synthetic fixture (6x2x2) | 1.50ms | complete on synthetic fixture | OPT-005 |
| SIBI | none | n/a | unsupported reader | |

## MCWorld End-to-End Record

The large real Schematic was converted into a fresh `.mcworld` archive three
times in the Release build. Each run unpacked an empty Bedrock world, parsed the
519x256x519 structure, streamed 1,089 chunks into LevelDB, closed the world, and
repacked the archive. Checksums were stable at `12819008811232`; the final
archive was 4,110,495 bytes.

| Run | Open/unpack | Write/close/repack | Total | Peak private memory |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 3.142ms | 1.746s | 3.643s | 412.375 MiB |
| 2 | 3.112ms | 1.696s | 3.541s | 412.379 MiB |
| 3 | 3.929ms | 1.737s | 3.631s | 412.371 MiB |

The total includes mapping, parsing, chunk streaming, entity streaming, world
open, LevelDB close, and archive repacking; it excludes process startup and the
creation of the empty seed archive.

The former optimized fixed-width Release end-to-end run reported `parse_ms` = 9.168s,
`get_chunks_ms` = 33.900s, world write/close/repack = 49.017s, total
`92.863s`, stable checksum `393687046530647`, and peak working set 167.566 MiB.
The resident working set is below the 500 MiB goal. The Linux compatibility
counter's `VmPeak`-based virtual private figure was 521.625 MiB; this is slightly
above the private-memory gate and is dominated by native Bedrock writer virtual
allocations rather than resident Schem buffers.

The post-OPT-009 CLI result is the preferred production measure for Schem to a
world directory. It does not include `.mcworld` archive repacking.

## Verification Note

Large output validation has three tiers: a source-stream checksum during an
optimized writer experiment, sampled readback of boundary and representative
chunks, and full reverse conversion only for smaller fixtures or scheduled
integration runs. The Release core test suite passes, including SchemV1/V2
writer round trips, negative-offset boundaries, extra-varint rejection, and the
Flight checksum.

## Benchmark Record Template

Record the command, commit, CPU/memory, build flags, three repetitions, median,
maximum, peak memory, output hash, and correctness checks for every completed
method.
