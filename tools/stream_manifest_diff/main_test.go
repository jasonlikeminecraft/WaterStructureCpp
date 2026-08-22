package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"testing"
)

type testManifestOptions struct {
	chunks           int
	entityCount      int
	layerMismatch    bool
	entityMismatch   bool
	entitiesFirst    bool
	entityBatchReset bool
	inputSHA         string
}

func writeTestManifest(t *testing.T, path string, options testManifestOptions) {
	t.Helper()
	file, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	writer := bufio.NewWriterSize(file, 64*1024)
	inputSHA := options.inputSHA
	if inputSHA == "" {
		inputSHA = "same"
	}
	fmt.Fprintf(writer, `{"schema":1,"block_hash_algorithm":"canonical-block-state-v1","nbt_hash_algorithm":"canonical-nbt-v1","input_sha256":%q,"format":"fixture","size":[32,16,32],"offset":[0,0,0],"non_air_blocks":1`, inputSHA)
	if options.entitiesFirst {
		writeTestEntities(writer, options)
	}
	fmt.Fprint(writer, `,"chunks":[`)
	for index := 0; index < options.chunks; index++ {
		if index != 0 {
			fmt.Fprint(writer, ",")
		}
		x, z := index/100, index%100
		layer := "same-layer"
		if options.layerMismatch && index == 1 {
			layer = "different-layer"
		}
		fmt.Fprintf(writer, `{"x":%d,"z":%d,"subchunks":[{"y":0,"layer0_sha256":%q,"layer1_sha256":"same-layer-1"}]}`, x, z, layer)
	}
	fmt.Fprint(writer, `]`)
	if !options.entitiesFirst {
		writeTestEntities(writer, options)
	}
	fmt.Fprint(writer, "}\n")
	if err := writer.Flush(); err != nil {
		t.Fatal(err)
	}
}

func writeTestEntities(writer *bufio.Writer, options testManifestOptions) {
	fmt.Fprint(writer, `,"block_entities":[`)
	for index := 0; index < options.entityCount; index++ {
		if index != 0 {
			fmt.Fprint(writer, ",")
		}
		x, z := (index/100)*16, (index%100)*16
		if options.entityBatchReset {
			x, z = (index%2)*16, 0
		}
		hash := "entity-hash"
		if options.entityMismatch && index == 1 {
			hash = "different-entity-hash"
		}
		fmt.Fprintf(writer, `{"x":%d,"y":0,"z":%d,"nbt_sha256":%q,"nbt_fields":[{"path":"/id","type":8,"value_sha256":"id-hash"}]}`, x, z, hash)
	}
	fmt.Fprint(writer, "]")
}

func TestEntityBatchCoordinatesMayRestart(t *testing.T) {
	directory := t.TempDir()
	goPath := filepath.Join(directory, "go.json")
	cppPath := filepath.Join(directory, "cpp.json")
	options := testManifestOptions{chunks: 4, entityCount: 8, entityBatchReset: true}
	writeTestManifest(t, goPath, options)
	writeTestManifest(t, cppPath, options)
	report, err := comparePaths(goPath, cppPath)
	if err != nil {
		t.Fatal(err)
	}
	if !report.Match || report.DifferenceCount != 0 {
		t.Fatalf("batch-reset report = %#v", report)
	}
}

func TestTopLevelArrayOrderDoesNotMatter(t *testing.T) {
	directory := t.TempDir()
	goPath := filepath.Join(directory, "go.json")
	cppPath := filepath.Join(directory, "cpp.json")
	writeTestManifest(t, goPath, testManifestOptions{chunks: 4, entityCount: 4})
	writeTestManifest(t, cppPath, testManifestOptions{chunks: 4, entityCount: 4, entitiesFirst: true})
	report, err := comparePaths(goPath, cppPath)
	if err != nil {
		t.Fatal(err)
	}
	if !report.Match || report.DifferenceCount != 0 {
		t.Fatalf("reordered report = %#v", report)
	}
}

func TestEqualManifestIsStreamingComparable(t *testing.T) {
	directory := t.TempDir()
	goPath := filepath.Join(directory, "go.json")
	cppPath := filepath.Join(directory, "cpp.json")
	options := testManifestOptions{chunks: 4, entityCount: 4}
	writeTestManifest(t, goPath, options)
	writeTestManifest(t, cppPath, options)
	report, err := comparePaths(goPath, cppPath)
	if err != nil {
		t.Fatal(err)
	}
	if !report.Match || report.DifferenceCount != 0 {
		t.Fatalf("equal report = %#v", report)
	}
}

func TestSemanticComparisonCanIgnoreInputSHA(t *testing.T) {
	directory := t.TempDir()
	firstPath := filepath.Join(directory, "first.json")
	secondPath := filepath.Join(directory, "second.json")
	writeTestManifest(t, firstPath, testManifestOptions{chunks: 4, entityCount: 4, inputSHA: "first"})
	writeTestManifest(t, secondPath, testManifestOptions{chunks: 4, entityCount: 4, inputSHA: "second"})

	strict, err := comparePaths(firstPath, secondPath)
	if err != nil {
		t.Fatal(err)
	}
	if strict.Match {
		t.Fatal("strict comparison unexpectedly ignored input_sha256")
	}
	semantic, err := comparePathsWithOptions(firstPath, secondPath, true)
	if err != nil {
		t.Fatal(err)
	}
	if !semantic.Match || semantic.DifferenceCount != 0 {
		t.Fatalf("semantic report = %#v", semantic)
	}
}

func TestLayerAndEntityMismatchReportFirstLayer(t *testing.T) {
	directory := t.TempDir()
	goPath := filepath.Join(directory, "go.json")
	cppPath := filepath.Join(directory, "cpp.json")
	writeTestManifest(t, goPath, testManifestOptions{chunks: 4, entityCount: 4, layerMismatch: true, entityMismatch: true})
	writeTestManifest(t, cppPath, testManifestOptions{chunks: 4, entityCount: 4})
	report, err := comparePaths(goPath, cppPath)
	if err != nil {
		t.Fatal(err)
	}
	if report.Match || report.DifferenceCount < 2 {
		t.Fatalf("mismatch report = %#v", report)
	}
	if report.FirstBlockMismatch == nil || report.FirstBlockMismatch.ChunkX != 0 || report.FirstBlockMismatch.ChunkZ != 1 || report.FirstBlockMismatch.Layer != 0 {
		t.Fatalf("first mismatch = %#v", report.FirstBlockMismatch)
	}
}

func TestLargeArraysDoNotBecomeSlices(t *testing.T) {
	directory := t.TempDir()
	goPath := filepath.Join(directory, "go.json")
	cppPath := filepath.Join(directory, "cpp.json")
	options := testManifestOptions{chunks: 100_000, entityCount: 10_000}
	writeTestManifest(t, goPath, options)
	writeTestManifest(t, cppPath, options)
	report, err := comparePaths(goPath, cppPath)
	if err != nil {
		t.Fatal(err)
	}
	if !report.Match {
		t.Fatalf("large equal report = %#v", report)
	}
}

func TestReportIsCompactJSON(t *testing.T) {
	report := comparisonReport{Match: false, DifferenceCount: 101, Differences: []string{"one"}, FirstBlockMismatch: &blockMismatch{ChunkX: 2, ChunkZ: 3, SubY: 4, Layer: 1}}
	data, err := json.Marshal(report)
	if err != nil {
		t.Fatal(err)
	}
	var decoded comparisonReport
	if err := json.Unmarshal(data, &decoded); err != nil {
		t.Fatal(err)
	}
	if decoded.DifferenceCount != 101 || decoded.FirstBlockMismatch == nil {
		t.Fatalf("decoded report = %#v", decoded)
	}
}
