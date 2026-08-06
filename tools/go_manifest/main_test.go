package main

import (
	"bytes"
	"os"
	"testing"

	"github.com/Yeah114/WaterStructure/structure"
)

func TestCanonicalBlockStateSortsPropertiesAndPreservesTypes(t *testing.T) {
	first := canonicalBlockState{
		Name:    "minecraft:test",
		Version: 42,
		Properties: []blockProperty{
			{Name: "second", Type: 8, Value: "b"},
			{Name: "first", Type: 3, Value: int32(7)},
		},
	}
	second := canonicalBlockState{
		Name:    first.Name,
		Version: first.Version,
		Properties: []blockProperty{
			first.Properties[1],
			first.Properties[0],
		},
	}
	left, err := canonicalBlockBytes(first)
	if err != nil {
		t.Fatal(err)
	}
	right, err := canonicalBlockBytes(second)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(left, right) {
		t.Fatal("property order changed canonical block encoding")
	}

	stringState := first
	stringState.Properties = []blockProperty{{Name: "first", Type: 8, Value: "7"}}
	stringBytes, err := canonicalBlockBytes(stringState)
	if err != nil {
		t.Fatal(err)
	}
	if bytes.Equal(left, stringBytes) {
		t.Fatal("property type missing from canonical block encoding")
	}
}

func TestCanonicalNBTSortsCompoundKeysAndPreservesTypes(t *testing.T) {
	encoded, err := canonicalNBT(map[string]any{"z": int32(9), "a": int8(1)})
	if err != nil {
		t.Fatal(err)
	}
	expected := []byte{
		'W', 'S', 'N', 'B', 1,
		10, 2, 0, 0, 0,
		1, 0, 0, 0, 'a', 1, 1,
		1, 0, 0, 0, 'z', 3, 9, 0, 0, 0,
	}
	if !bytes.Equal(encoded, expected) {
		t.Fatalf("canonical NBT mismatch:\n got %x\nwant %x", encoded, expected)
	}
}

func TestCanonicalNBTPreservesFixedArrayTypes(t *testing.T) {
	fixed := map[string]any{
		"bytes": [2]byte{1, 255},
		"ints":  [2]int32{1, -2},
		"longs": [2]int64{3, -4},
	}
	slices := map[string]any{
		"bytes": []byte{1, 255},
		"ints":  []int32{1, -2},
		"longs": []int64{3, -4},
	}
	fixedBytes, err := canonicalNBT(fixed)
	if err != nil {
		t.Fatal(err)
	}
	sliceBytes, err := canonicalNBT(slices)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(fixedBytes, sliceBytes) {
		t.Fatalf("fixed NBT arrays changed canonical types:\n fixed %x\nslices %x", fixedBytes, sliceBytes)
	}
}

func TestCanonicalNBTFieldsUseJSONPointerPaths(t *testing.T) {
	fields, err := canonicalNBTFields(map[string]any{
		"nested": map[string]any{"z": int32(9)},
		"a/b~c":  int8(1),
	})
	if err != nil {
		t.Fatal(err)
	}
	want := []struct {
		path   string
		typeID byte
	}{
		{"/a~1b~0c", 1},
		{"/nested", 10},
		{"/nested/z", 3},
	}
	if len(fields) != len(want) {
		t.Fatalf("field count = %d, want %d", len(fields), len(want))
	}
	for index, expected := range want {
		if fields[index].Path != expected.path || fields[index].Type != expected.typeID || fields[index].Value == "" {
			t.Fatalf("field %d = %#v, want path=%s type=%d", index, fields[index], expected.path, expected.typeID)
		}
	}
}

func TestParseOptionsSupportsForcedFormatAndDetail(t *testing.T) {
	detail, format, err := parseOptions([]string{
		"--format", "GangBanV3", "--detail", "-1", "2", "3", "1",
	})
	if err != nil {
		t.Fatal(err)
	}
	if format != "GangBanV3" || detail == nil || detail.chunkX != -1 ||
		detail.chunkZ != 2 || detail.subY != 3 || detail.layer != 1 {
		t.Fatalf("format=%q detail=%#v", format, detail)
	}
}

func TestNormalizeSchematicOracleOffsets(t *testing.T) {
	reader := &structure.Schematic{
		BlocksTagGzipOffset: 101,
		DataTagGzipOffset:   205,
	}
	if normalized := normalizeOracleReader(reader); normalized != reader {
		t.Fatal("normalization replaced the oracle reader")
	}
	if reader.BlocksTagGzipOffset != 105 || reader.DataTagGzipOffset != 209 {
		t.Fatalf("normalized offsets = (%d, %d), want (105, 209)",
			reader.BlocksTagGzipOffset, reader.DataTagGzipOffset)
	}
}

func TestGangBanV2OracleInfersMissingRange(t *testing.T) {
	file, err := os.CreateTemp("", "gangban-v2-oracle-*.json")
	if err != nil {
		t.Fatal(err)
	}
	path := file.Name()
	defer os.Remove(path)
	defer file.Close()
	fixture := `[{"id":1,"aux":0,"p":[-2,3,4]},{"id":1,"aux":0,"p":[1,5,8]},{"list":["minecraft:air","minecraft:stone"]}]`
	if _, err := file.WriteString(fixture); err != nil {
		t.Fatal(err)
	}
	reader, err := openGangBanV2Oracle(file)
	if err != nil {
		t.Fatal(err)
	}
	size := reader.GetSize()
	if reader.Name() != "GangBanV2" || size.GetWidth() != 4 ||
		size.GetHeight() != 3 || size.GetLength() != 5 {
		t.Fatalf("name=%s size=%dx%dx%d", reader.Name(), size.GetWidth(),
			size.GetHeight(), size.GetLength())
	}
}
