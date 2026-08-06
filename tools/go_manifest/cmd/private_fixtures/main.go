package main

import (
	"bytes"
	"compress/flate"
	"crypto/md5"
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"

	"github.com/vmihailenco/msgpack/v5"
)

func main() {
	output := flag.String("output", "", "directory for generated private-format fixtures")
	flag.Parse()
	if *output == "" {
		fmt.Fprintln(os.Stderr, "-output is required")
		os.Exit(2)
	}
	if err := generate(*output); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func generate(output string) error {
	if err := os.MkdirAll(output, 0o755); err != nil {
		return err
	}
	fixtures := map[string][]byte{}

	bds, err := msgpack.Marshal([]any{[]any{
		[]any{"minecraft:stone", int32(-2), int32(3), int32(4), int32(0), false},
		[]any{" MINECRAFT:AIR ", int32(-1), int32(3), int32(4), int32(0), false},
		[]any{"minecraft:dirt", int32(-1), int32(3), int32(4), float64(0), false},
		[]any{"minecraft:oak_log", int32(0), int32(3), int32(4), "[axis=x]", false, nil},
	}})
	if err != nil {
		return err
	}
	fixtures["bds-minimal.bds"] = bds

	nexus, err := msgpack.Marshal([]any{[]any{
		[]any{"minecraft:stone", int32(-2), int32(3), int32(4), int32(0)},
		[]any{" MINECRAFT:AIR ", int32(-1), int32(3), int32(4), int32(0)},
		[]any{"minecraft:dirt", int32(-1), int32(3), int32(4), int32(0)},
	}, []any{}})
	if err != nil {
		return err
	}
	fixtures["nexus-minimal.np"] = nexus
	fixtures["bcf-minimal.bcf"] = bcfFixture()
	tibi, err := tibiFixture()
	if err != nil {
		return err
	}
	fixtures["tibi-minimal.tibi"] = tibi

	cov, err := json.Marshal(map[string]any{
		"size": []int{2, 1, 1},
		"structure": map[string]any{
			"palette": []any{
				map[string]any{"val": 0, "name": "minecraft:stone", "data": 0},
				map[string]any{"val": 1, "name": "minecraft:air"},
				map[string]any{"val": 2, "name": "minecraft:dirt", "data": 0},
			},
			"block_indices": []any{[]any{[]int{0, 1, 2}}},
		},
	})
	if err != nil {
		return err
	}
	fixtures["cov-minimal.covstructure"] = cov

	fuhongV1, err := json.Marshal([]any{
		map[string]any{"name": "minecraft:stone", "aux": 0, "x": []int{-2, 99}, "y": 3, "z": 4},
		map[string]any{"name": "minecraft:air", "aux": 0, "x": -1, "y": 3, "z": 4},
		map[string]any{"name": "minecraft:stone", "aux": 0, "x": -1, "y": 3, "z": 4},
	})
	if err != nil {
		return err
	}
	fixtures["fuhong-v1-minimal.json"] = fuhongV1

	fuhongV2, err := json.Marshal(map[string]any{
		"Build_Info": map[string]any{},
		"FuHongBuild_FinalFormat": []any{map[string]any{"block": []any{
			map[string]any{
				"n": "minecraft:chest", "a": []int{0},
				"x": []int{-1}, "y": []int{2}, "z": []int{3},
				"d": []any{map[string]any{"d": []any{map[string]any{
					"name": "stone", "damage": 0, "count": 1, "slot": 0,
				}}}},
			},
			map[string]any{
				"n": "minecraft:command_block", "a": 0,
				"x": []int{0}, "y": []int{2}, "z": []int{3},
				"state": []string{"conditional_bit=true"},
				"c": map[string]any{
					"c": []string{"say v2"}, "t": []int{2},
					"a": []bool{true}, "n": []string{"Tester"},
				},
			},
		}}},
	})
	if err != nil {
		return err
	}
	fixtures["fuhong-v2-minimal.json"] = fuhongV2
	fixtures["sibi-unsupported.sibi"] = []byte("H4")

	for name, data := range fixtures {
		path := filepath.Join(output, name)
		if err := os.WriteFile(path, data, 0o644); err != nil {
			return err
		}
		if name == "sibi-unsupported.sibi" {
			continue
		}
		truncated := append([]byte(nil), data...)
		if len(truncated) != 0 {
			truncated = truncated[:len(truncated)-1]
		}
		ext := filepath.Ext(name)
		base := name[:len(name)-len(ext)]
		if err := os.WriteFile(filepath.Join(output, base+"-truncated"+ext), truncated, 0o644); err != nil {
			return err
		}
	}
	return nil
}

func bcfFixture() []byte {
	var data []byte
	u8 := func(value uint8) { data = append(data, value) }
	u16 := func(value uint16) { data = binary.LittleEndian.AppendUint16(data, value) }
	u32 := func(value uint32) { data = binary.LittleEndian.AppendUint32(data, value) }
	u64 := func(value uint64) { data = binary.LittleEndian.AppendUint64(data, value) }
	string16 := func(value string) {
		u16(uint16(len(value)))
		data = append(data, value...)
	}
	patch64 := func(offset int, value uint64) { binary.LittleEndian.PutUint64(data[offset:offset+8], value) }

	data = append(data, "BCF"...)
	u8(1)
	u16(3)
	u16(1)
	u16(1)
	u8(16)
	u64(1)
	pointers := len(data)
	for range 5 {
		u64(0)
	}
	section := len(data)
	u64(64)
	u16(0xfffe)
	u16(3)
	u16(4)
	u32(2)
	u32(1)
	for range 6 {
		u16(0)
	}
	u32(2)
	u16(2)
	u16(0)
	u16(0)
	u16(1)
	u16(0)
	u16(0)

	offsets := len(data)
	u64(1)
	u64(uint64(section))
	types := len(data)
	u32(2)
	u16(1)
	string16("minecraft:stone")
	u16(2)
	string16("minecraft:dirt")
	stateNames := len(data)
	u32(0)
	stateValues := len(data)
	u32(0)
	palette := len(data)
	u32(2)
	u32(1)
	u16(1)
	u16(0)
	u32(2)
	u16(2)
	u16(0)

	patch64(pointers, uint64(offsets))
	patch64(pointers+8, uint64(palette))
	patch64(pointers+16, uint64(types))
	patch64(pointers+24, uint64(stateNames))
	patch64(pointers+32, uint64(stateValues))
	return data
}

func tibiFixture() ([]byte, error) {
	var payload []byte
	varint := func(value uint64) { payload = binary.AppendUvarint(payload, value) }
	stringValue := func(value string) {
		varint(uint64(len(value)))
		payload = append(payload, value...)
	}
	varint(2)
	varint(0)
	stringValue("minecraft:stone")
	varint(1)
	stringValue("minecraft:dirt")
	varint(1)
	varint(0)
	stringValue("")
	varint(3)
	varint(0)
	varint(0)
	varint(5)
	varint(7)
	varint(9)
	varint(0)
	varint(1)
	varint(1)
	varint(6)
	varint(7)
	varint(9)
	varint(7)
	varint(8)
	varint(10)
	varint(0)
	varint(1)
	varint(0)
	varint(10)
	varint(7)
	varint(9)
	varint(8)
	varint(7)
	varint(9)
	varint(0)

	header := []byte("TIBI-HEADER-001")
	keyInput := append(append([]byte(nil), header...), []byte(fmt.Sprintf("TIBI_2025/5/19-Start%d", len(payload)))...)
	key := md5.Sum(keyInput)
	decoded := append([]byte(nil), header...)
	for index, value := range payload {
		decoded = append(decoded, value^key[index%len(key)])
	}
	var compressed bytes.Buffer
	writer, err := flate.NewWriter(&compressed, flate.DefaultCompression)
	if err != nil {
		return nil, err
	}
	if _, err := writer.Write(decoded); err != nil {
		return nil, err
	}
	if err := writer.Close(); err != nil {
		return nil, err
	}
	return compressed.Bytes(), nil
}
