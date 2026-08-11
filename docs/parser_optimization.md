# Parser Optimization Log

This document tracks parser performance work and reusable techniques. Results use
Release builds (`-O3 -DNDEBUG`) and are recorded separately for parsing and for
end-to-end conversion.

## Acceptance

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

### OPT-007: Streaming Schem BlockData with fixed-width stripe access

**Status:** implemented and verified on the newly uploaded 41 MiB Flight sample.

SchemV1/V2 no longer materialize the complete NBT tree, byte array, or packed
block-index vector. The gzip payload is copied to a temporary file. On first chunk
access, varints are converted once into a temporary fixed-width index file (1, 2,
or 4 bytes per palette index); subsequent chunk reads use buffered fixed-width
values. The Schem visitor consumes a whole bounded batch, avoiding the compatibility
visitor's one-chunk fallback.

The sample is `2610x282x2615` (26,896 chunks, 510,146,162 non-air blocks). The
The final fixed-width Release run reported `parse_ms` = 8.229s,
`get_chunks_ms` = 34.503s, total `43.573s`, checksum `393687046530647`,
peak working set 138.219 MiB, and peak private usage 147.551 MiB. This meets
both the 25-second parser target and the 500 MiB memory gate. Full-Z stripe
batches avoid rereading source rows, exact row reads avoid 1 MiB read-ahead for
5 KiB rows, and a 1 MiB output buffer replaces per-index stream writes.

### OPT-008: Batch Bedrock subchunk writes and omit empty layer1

**Status:** implemented and verified on Schematic and the Flight Schem sample.

`BedrockWorldAdapter` now collects all subchunks of a chunk into the existing
`BedrockWorldOperator::World::saveSubChunksBatch()` call. This replaces a
metadata write plus a data write for every subchunk with one LevelDB batch per
chunk. The adapter also checks whether layer1 contains a non-air runtime ID;
the normally empty secondary layer is no longer transposed, allocated in BWO,
or passed to its encoder. Formats with a populated secondary layer retain it.

On Flight, the previous fixed-index world stage was 49.017s. With this method
it was 35.734s: 10.199s in save batches and 25.529s in generic chunk
materialization. The remaining materialization cost motivated OPT-009.

### OPT-009: Direct Schem 16-layer slab-to-world stream

**Status:** implemented and verified by a full source-stream checksum.

For direct Schem-to-world conversion, SchemV1/V2 with zero block offset and a
palette index range no wider than 16 bits bypass `get_chunks()` and the 3.85 GiB
fixed-width index file. The reader retains one 16-Y-layer palette-index slab;
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

The optimized fixed-width Release end-to-end run reported `parse_ms` = 9.168s,
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
