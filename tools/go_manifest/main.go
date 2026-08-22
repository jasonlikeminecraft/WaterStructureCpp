package main

import (
	"archive/zip"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"reflect"
	"runtime"
	"sort"
	"strconv"
	"strings"

	"github.com/TriM-Organization/bedrock-world-operator/block"
	"github.com/Yeah114/WaterStructure/define"
	"github.com/Yeah114/WaterStructure/structure"
	legacyblocks "github.com/Yeah114/blocks"
)

type subManifest struct {
	Y      int    `json:"y"`
	Layer0 string `json:"layer0_sha256"`
	Layer1 string `json:"layer1_sha256"`
}

type chunkManifest struct {
	X         int32         `json:"x"`
	Z         int32         `json:"z"`
	Subchunks []subManifest `json:"subchunks"`
}

type entityManifest struct {
	X      int32              `json:"x"`
	Y      int32              `json:"y"`
	Z      int32              `json:"z"`
	Hash   string             `json:"nbt_sha256"`
	Fields []nbtFieldManifest `json:"nbt_fields"`
}

type nbtFieldManifest struct {
	Path  string `json:"path"`
	Type  byte   `json:"type"`
	Value string `json:"value_sha256"`
}

type manifest struct {
	Schema             int              `json:"schema"`
	BlockHashAlgorithm string           `json:"block_hash_algorithm"`
	NBTHashAlgorithm   string           `json:"nbt_hash_algorithm"`
	InputSHA256        string           `json:"input_sha256"`
	Format             string           `json:"format,omitempty"`
	Size               []int            `json:"size,omitempty"`
	Offset             []int32          `json:"offset,omitempty"`
	NonAir             int              `json:"non_air_blocks,omitempty"`
	Chunks             []chunkManifest  `json:"chunks,omitempty"`
	BlockEntities      []entityManifest `json:"block_entities,omitempty"`
	Detail             any              `json:"detail,omitempty"`
	Error              any              `json:"error,omitempty"`
}

// jsonSpool keeps the potentially unbounded chunk/entity arrays on disk.  A
// manifest is a diagnostic artifact, so retaining every entry in a Go slice
// is needlessly expensive (large worlds previously reached multi-gigabyte
// heaps).  Each entry is encoded once and copied into the final JSON stream.
type jsonSpool struct {
	file  *os.File
	path  string
	first bool
	count uint64
}

func newJSONSpool(prefix string) (*jsonSpool, error) {
	file, err := os.CreateTemp("", "water-structure-"+prefix+"-*.jsonpart")
	if err != nil {
		return nil, err
	}
	return &jsonSpool{file: file, path: file.Name(), first: true}, nil
}

func (s *jsonSpool) append(value any) error {
	if !s.first {
		if _, err := s.file.WriteString(","); err != nil {
			return err
		}
	}
	encoded, err := json.Marshal(value)
	if err != nil {
		return err
	}
	if _, err = s.file.Write(encoded); err != nil {
		return err
	}
	s.first = false
	s.count++
	return nil
}

func (s *jsonSpool) close() error {
	if s == nil || s.file == nil {
		return nil
	}
	err := s.file.Close()
	s.file = nil
	return err
}

func (s *jsonSpool) cleanup() {
	if s == nil {
		return
	}
	_ = s.close()
	_ = os.Remove(s.path)
}

func copySpool(output io.Writer, s *jsonSpool) error {
	if s == nil {
		return nil
	}
	if err := s.close(); err != nil {
		return err
	}
	input, err := os.Open(s.path)
	if err != nil {
		return err
	}
	defer input.Close()
	_, err = io.Copy(output, input)
	return err
}

func writeManifest(path string, result manifest, chunks, entities *jsonSpool, includeArrays bool) error {
	result.Chunks = nil
	result.BlockEntities = nil
	encoded, err := json.Marshal(result)
	if err != nil {
		return err
	}
	if len(encoded) == 0 || encoded[len(encoded)-1] != '}' {
		return fmt.Errorf("manifest metadata did not encode as an object")
	}
	output := os.Stdout
	var file *os.File
	if path != "" {
		file, err = os.OpenFile(path, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
		if err != nil {
			return err
		}
		defer file.Close()
		output = file
	}
	if _, err = output.Write(encoded[:len(encoded)-1]); err != nil {
		return err
	}
	if includeArrays {
		if _, err = io.WriteString(output, `,"chunks":[`); err != nil {
			return err
		}
		if err = copySpool(output, chunks); err != nil {
			return err
		}
		if _, err = io.WriteString(output, `],"block_entities":[`); err != nil {
			return err
		}
		if err = copySpool(output, entities); err != nil {
			return err
		}
		if _, err = io.WriteString(output, "]"); err != nil {
			return err
		}
	}
	_, err = io.WriteString(output, "}\n")
	return err
}

type blockProperty struct {
	Name  string
	Type  byte
	Value any
}

type canonicalBlockState struct {
	Name       string
	Version    int32
	Properties []blockProperty
}

type mappingProperty struct {
	Type  string `json:"type"`
	Value string `json:"value"`
}

type mappingEntry struct {
	Name    string                     `json:"name"`
	States  map[string]mappingProperty `json:"states"`
	Version int32                      `json:"version"`
}

type mappingFile struct {
	Schema  int            `json:"schema"`
	Palette []mappingEntry `json:"palette"`
}

func digest(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

func inputDigest(path string) (string, error) {
	info, err := os.Stat(path)
	if err != nil {
		return "", err
	}
	if !info.IsDir() {
		file, err := os.Open(path)
		if err != nil {
			return "", err
		}
		hash := sha256.New()
		_, copyErr := io.Copy(hash, file)
		closeErr := file.Close()
		if copyErr != nil {
			return "", copyErr
		}
		if closeErr != nil {
			return "", closeErr
		}
		return hex.EncodeToString(hash.Sum(nil)), nil
	}

	type directoryEntry struct {
		path     string
		relative string
		size     int64
	}
	entries := make([]directoryEntry, 0)
	err = filepath.WalkDir(path, func(entryPath string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entryPath == path {
			return nil
		}
		if entry.Type()&os.ModeSymlink != 0 {
			return fmt.Errorf("directory input contains symlink: %s", entryPath)
		}
		if entry.IsDir() {
			return nil
		}
		entryInfo, err := entry.Info()
		if err != nil {
			return err
		}
		if !entryInfo.Mode().IsRegular() {
			return nil
		}
		relative, err := filepath.Rel(path, entryPath)
		if err != nil {
			return err
		}
		entries = append(entries, directoryEntry{entryPath, filepath.ToSlash(relative), entryInfo.Size()})
		return nil
	})
	if err != nil {
		return "", err
	}
	sort.Slice(entries, func(i, j int) bool { return entries[i].relative < entries[j].relative })

	hash := sha256.New()
	_, _ = hash.Write([]byte("WS-DIR-SHA256-V1\x00"))
	var encoded [8]byte
	for _, entry := range entries {
		if uint64(len(entry.relative)) > math.MaxUint32 {
			return "", fmt.Errorf("directory relative path is too long")
		}
		binary.LittleEndian.PutUint32(encoded[:4], uint32(len(entry.relative)))
		_, _ = hash.Write(encoded[:4])
		_, _ = hash.Write([]byte(entry.relative))
		binary.LittleEndian.PutUint64(encoded[:], uint64(entry.size))
		_, _ = hash.Write(encoded[:])
		file, err := os.Open(entry.path)
		if err != nil {
			return "", err
		}
		written, copyErr := io.Copy(hash, file)
		closeErr := file.Close()
		if copyErr != nil {
			return "", copyErr
		}
		if closeErr != nil {
			return "", closeErr
		}
		if written != entry.size {
			return "", fmt.Errorf("directory file changed while hashing: %s", entry.path)
		}
	}
	return hex.EncodeToString(hash.Sum(nil)), nil
}

func zipDirectory(source, target string) error {
	output, err := os.Create(target)
	if err != nil {
		return err
	}
	archive := zip.NewWriter(output)
	walkErr := filepath.WalkDir(source, func(path string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if path == source || entry.IsDir() {
			return nil
		}
		if entry.Type()&os.ModeSymlink != 0 {
			return fmt.Errorf("directory input contains symlink: %s", path)
		}
		info, err := entry.Info()
		if err != nil {
			return err
		}
		if !info.Mode().IsRegular() {
			return nil
		}
		relative, err := filepath.Rel(source, path)
		if err != nil {
			return err
		}
		header, err := zip.FileInfoHeader(info)
		if err != nil {
			return err
		}
		header.Name = filepath.ToSlash(relative)
		header.Method = zip.Deflate
		writer, err := archive.CreateHeader(header)
		if err != nil {
			return err
		}
		input, err := os.Open(path)
		if err != nil {
			return err
		}
		_, copyErr := io.Copy(writer, input)
		closeErr := input.Close()
		if copyErr != nil {
			return copyErr
		}
		return closeErr
	})
	archiveErr := archive.Close()
	closeErr := output.Close()
	if walkErr != nil {
		return walkErr
	}
	if archiveErr != nil {
		return archiveErr
	}
	return closeErr
}

func openStructureInput(path string) (*os.File, func(), error) {
	info, err := os.Stat(path)
	if err != nil {
		return nil, func() {}, err
	}
	if !info.IsDir() {
		file, err := os.Open(path)
		return file, func() {}, err
	}
	temporary, err := os.MkdirTemp("", "go_manifest_world_*")
	if err != nil {
		return nil, func() {}, err
	}
	cleanup := func() { _ = os.RemoveAll(temporary) }
	archivePath := filepath.Join(temporary, filepath.Base(filepath.Clean(path))+".mcworld")
	if err := zipDirectory(path, archivePath); err != nil {
		cleanup()
		return nil, func() {}, err
	}
	file, err := os.Open(archivePath)
	if err != nil {
		cleanup()
		return nil, func() {}, err
	}
	return file, cleanup, nil
}

func appendString(data []byte, value string) []byte {
	data = binary.LittleEndian.AppendUint32(data, uint32(len(value)))
	return append(data, value...)
}

func appendProperty(data []byte, property blockProperty) ([]byte, error) {
	data = appendString(data, property.Name)
	data = append(data, property.Type)
	switch property.Type {
	case 1:
		switch value := property.Value.(type) {
		case int8:
			data = append(data, byte(value))
		case uint8:
			data = append(data, value)
		case bool:
			if value {
				data = append(data, 1)
			} else {
				data = append(data, 0)
			}
		default:
			return nil, fmt.Errorf("byte state %s has type %T", property.Name, property.Value)
		}
	case 2:
		value, ok := property.Value.(int16)
		if !ok {
			return nil, fmt.Errorf("short state %s has type %T", property.Name, property.Value)
		}
		data = binary.LittleEndian.AppendUint16(data, uint16(value))
	case 3:
		value, ok := property.Value.(int32)
		if !ok {
			return nil, fmt.Errorf("int state %s has type %T", property.Name, property.Value)
		}
		data = binary.LittleEndian.AppendUint32(data, uint32(value))
	case 4:
		value, ok := property.Value.(int64)
		if !ok {
			return nil, fmt.Errorf("long state %s has type %T", property.Name, property.Value)
		}
		data = binary.LittleEndian.AppendUint64(data, uint64(value))
	case 8:
		value, ok := property.Value.(string)
		if !ok {
			return nil, fmt.Errorf("string state %s has type %T", property.Name, property.Value)
		}
		data = appendString(data, value)
	default:
		return nil, fmt.Errorf("unknown block state type %d", property.Type)
	}
	return data, nil
}

func canonicalBlockBytes(state canonicalBlockState) ([]byte, error) {
	data := []byte{'W', 'S', 'B', 'S', 1}
	data = appendString(data, state.Name)
	data = binary.LittleEndian.AppendUint32(data, uint32(state.Version))
	properties := append([]blockProperty(nil), state.Properties...)
	sort.Slice(properties, func(i, j int) bool {
		if properties[i].Name != properties[j].Name {
			return properties[i].Name < properties[j].Name
		}
		if properties[i].Type != properties[j].Type {
			return properties[i].Type < properties[j].Type
		}
		return fmt.Sprint(properties[i].Value) < fmt.Sprint(properties[j].Value)
	})
	data = binary.LittleEndian.AppendUint32(data, uint32(len(properties)))
	var err error
	for _, property := range properties {
		data, err = appendProperty(data, property)
		if err != nil {
			return nil, err
		}
	}
	return data, nil
}

func propertiesFromRuntime(values map[string]any) ([]blockProperty, error) {
	properties := make([]blockProperty, 0, len(values))
	for name, value := range values {
		property := blockProperty{Name: name, Value: value}
		switch value.(type) {
		case int8, uint8, bool:
			property.Type = 1
		case int16:
			property.Type = 2
		case int32:
			property.Type = 3
		case int64:
			property.Type = 4
		case string:
			property.Type = 8
		default:
			return nil, fmt.Errorf("unsupported block state %s type %T", name, value)
		}
		properties = append(properties, property)
	}
	return properties, nil
}

func mappingLookupKey(name string, properties []blockProperty) (string, error) {
	state := canonicalBlockState{Name: name, Properties: properties}
	encoded, err := canonicalBlockBytes(state)
	if err != nil {
		return "", err
	}
	// Version occupies four bytes immediately after the name. It is zero here for every lookup.
	return string(encoded), nil
}

func findMapping() (string, error) {
	starts := []string{}
	if executable, err := os.Executable(); err == nil {
		starts = append(starts, filepath.Dir(executable))
	}
	if current, err := os.Getwd(); err == nil {
		starts = append(starts, current)
	}
	for _, start := range starts {
		directory := start
		for range 8 {
			candidate := filepath.Join(directory, "assets", "block_mappings_v1.json")
			if info, err := os.Stat(candidate); err == nil && !info.IsDir() {
				return candidate, nil
			}
			parent := filepath.Dir(directory)
			if parent == directory {
				break
			}
			directory = parent
		}
	}
	return "", fmt.Errorf("block_mappings_v1.json not found")
}

func loadVersions() (map[string]int32, error) {
	path, err := findMapping()
	if err != nil {
		return nil, err
	}
	contents, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var mappings mappingFile
	if err := json.Unmarshal(contents, &mappings); err != nil {
		return nil, err
	}
	if mappings.Schema != 1 {
		return nil, fmt.Errorf("unsupported mapping schema %d", mappings.Schema)
	}
	versions := make(map[string]int32, len(mappings.Palette))
	for _, entry := range mappings.Palette {
		properties := make([]blockProperty, 0, len(entry.States))
		for name, encoded := range entry.States {
			property := blockProperty{Name: name}
			switch encoded.Type {
			case "byte":
				property.Type = 1
				var parsed int
				if _, err := fmt.Sscan(encoded.Value, &parsed); err != nil {
					return nil, err
				}
				property.Value = uint8(parsed)
			case "short":
				property.Type = 2
				var parsed int16
				if _, err := fmt.Sscan(encoded.Value, &parsed); err != nil {
					return nil, err
				}
				property.Value = parsed
			case "int":
				property.Type = 3
				var parsed int32
				if _, err := fmt.Sscan(encoded.Value, &parsed); err != nil {
					return nil, err
				}
				property.Value = parsed
			case "long":
				property.Type = 4
				var parsed int64
				if _, err := fmt.Sscan(encoded.Value, &parsed); err != nil {
					return nil, err
				}
				property.Value = parsed
			case "string":
				property.Type = 8
				property.Value = encoded.Value
			default:
				return nil, fmt.Errorf("unknown mapping state type %q", encoded.Type)
			}
			properties = append(properties, property)
		}
		key, err := mappingLookupKey(entry.Name, properties)
		if err != nil {
			return nil, err
		}
		if previous, exists := versions[key]; exists && previous != entry.Version {
			return nil, fmt.Errorf("ambiguous block version for %s", entry.Name)
		}
		versions[key] = entry.Version
	}
	return versions, nil
}

func canonicalRuntimeBlock(runtimeID uint32, versions map[string]int32) ([]byte, error) {
	name, values, found := runtimeBlockState(runtimeID)
	if !found {
		return nil, fmt.Errorf("runtime ID %d has no block state", runtimeID)
	}
	properties, err := propertiesFromRuntime(values)
	if err != nil {
		return nil, err
	}
	key, err := mappingLookupKey(name, properties)
	if err != nil {
		return nil, err
	}
	version, found := versions[key]
	if !found {
		return nil, fmt.Errorf(
			"block state version not found for %s states=%v (runtime ID %d)",
			name, values, runtimeID)
	}
	return canonicalBlockBytes(canonicalBlockState{Name: name, Version: version, Properties: properties})
}

// Fatalder normally converts the legacy `github.com/Yeah114/blocks` runtime
// IDs to bedrock-world-operator IDs before materialising chunks. A few older
// command-block paths expose the legacy ID directly, however. The manifest is
// a semantic oracle, so resolve both registered ID spaces without changing the
// Fatalder source tree. Bedrock IDs take precedence if an integer happens to
// exist in both registries.
func runtimeBlockState(runtimeID uint32) (string, map[string]any, bool) {
	if name, values, found := block.RuntimeIDToState(runtimeID); found {
		return name, values, true
	}
	name, values, found := legacyblocks.RuntimeIDToState(runtimeID)
	if found && !strings.Contains(name, ":") {
		name = "minecraft:" + name
	}
	return name, values, found
}

func layerDigest(c interface {
	Block(uint8, int16, uint8, uint8) uint32
}, subY int, layer uint8, versions map[string]int32, cache map[uint32][]byte) (string, int, error) {
	data := make([]byte, 0, 4096*32)
	nonAir := 0
	for y := 0; y < 16; y++ {
		for z := 0; z < 16; z++ {
			for x := 0; x < 16; x++ {
				value := c.Block(uint8(x), int16(subY*16+y), uint8(z), layer)
				if value != block.AirRuntimeID {
					nonAir++
				}
				encoded, ok := cache[value]
				if !ok {
					var err error
					encoded, err = canonicalRuntimeBlock(value, versions)
					if err != nil {
						return "", 0, err
					}
					cache[value] = encoded
				}
				data = binary.LittleEndian.AppendUint32(data, uint32(len(encoded)))
				data = append(data, encoded...)
			}
		}
	}
	return digest(data), nonAir, nil
}

func appendCanonicalNBT(data []byte, value any) ([]byte, error) {
	if value == nil {
		return nil, fmt.Errorf("nil NBT value")
	}
	switch current := value.(type) {
	case int8:
		return append(data, 1, byte(current)), nil
	case uint8:
		return append(data, 1, current), nil
	case bool:
		if current {
			return append(data, 1, 1), nil
		}
		return append(data, 1, 0), nil
	case int16:
		data = append(data, 2)
		return binary.LittleEndian.AppendUint16(data, uint16(current)), nil
	case uint16:
		data = append(data, 2)
		return binary.LittleEndian.AppendUint16(data, current), nil
	case int32:
		data = append(data, 3)
		return binary.LittleEndian.AppendUint32(data, uint32(current)), nil
	case uint32:
		data = append(data, 3)
		return binary.LittleEndian.AppendUint32(data, current), nil
	case int:
		data = append(data, 4)
		return binary.LittleEndian.AppendUint64(data, uint64(current)), nil
	case int64:
		data = append(data, 4)
		return binary.LittleEndian.AppendUint64(data, uint64(current)), nil
	case uint64:
		data = append(data, 4)
		return binary.LittleEndian.AppendUint64(data, current), nil
	case float32:
		data = append(data, 5)
		return binary.LittleEndian.AppendUint32(data, math.Float32bits(current)), nil
	case float64:
		data = append(data, 6)
		return binary.LittleEndian.AppendUint64(data, math.Float64bits(current)), nil
	case []byte:
		data = append(data, 7)
		data = binary.LittleEndian.AppendUint32(data, uint32(len(current)))
		return append(data, current...), nil
	case []int8:
		data = append(data, 7)
		data = binary.LittleEndian.AppendUint32(data, uint32(len(current)))
		for _, item := range current {
			data = append(data, byte(item))
		}
		return data, nil
	case string:
		data = append(data, 8)
		return appendString(data, current), nil
	case []int32:
		data = append(data, 11)
		data = binary.LittleEndian.AppendUint32(data, uint32(len(current)))
		for _, item := range current {
			data = binary.LittleEndian.AppendUint32(data, uint32(item))
		}
		return data, nil
	case []int64:
		data = append(data, 12)
		data = binary.LittleEndian.AppendUint32(data, uint32(len(current)))
		for _, item := range current {
			data = binary.LittleEndian.AppendUint64(data, uint64(item))
		}
		return data, nil
	case map[string]any:
		keys := make([]string, 0, len(current))
		for key := range current {
			keys = append(keys, key)
		}
		sort.Strings(keys)
		data = append(data, 10)
		data = binary.LittleEndian.AppendUint32(data, uint32(len(keys)))
		var err error
		for _, key := range keys {
			data = appendString(data, key)
			data, err = appendCanonicalNBT(data, current[key])
			if err != nil {
				return nil, fmt.Errorf("%s: %w", key, err)
			}
		}
		return data, nil
	}
	reflected := reflect.ValueOf(value)
	for reflected.Kind() == reflect.Interface || reflected.Kind() == reflect.Pointer {
		if reflected.IsNil() {
			return nil, fmt.Errorf("nil NBT value")
		}
		reflected = reflected.Elem()
	}
	if reflected.Kind() == reflect.Array {
		switch reflected.Type().Elem().Kind() {
		case reflect.Int8, reflect.Uint8:
			data = append(data, 7)
			data = binary.LittleEndian.AppendUint32(data, uint32(reflected.Len()))
			for index := 0; index < reflected.Len(); index++ {
				item := reflected.Index(index)
				if item.Kind() == reflect.Uint8 {
					data = append(data, byte(item.Uint()))
				} else {
					data = append(data, byte(item.Int()))
				}
			}
			return data, nil
		case reflect.Int32:
			data = append(data, 11)
			data = binary.LittleEndian.AppendUint32(data, uint32(reflected.Len()))
			for index := 0; index < reflected.Len(); index++ {
				data = binary.LittleEndian.AppendUint32(data, uint32(reflected.Index(index).Int()))
			}
			return data, nil
		case reflect.Int64:
			data = append(data, 12)
			data = binary.LittleEndian.AppendUint32(data, uint32(reflected.Len()))
			for index := 0; index < reflected.Len(); index++ {
				data = binary.LittleEndian.AppendUint64(data, uint64(reflected.Index(index).Int()))
			}
			return data, nil
		}
	}
	switch reflected.Kind() {
	case reflect.Map:
		if reflected.Type().Key().Kind() != reflect.String {
			return nil, fmt.Errorf("NBT map key type %s", reflected.Type().Key())
		}
		keys := reflected.MapKeys()
		sort.Slice(keys, func(i, j int) bool { return keys[i].String() < keys[j].String() })
		data = append(data, 10)
		data = binary.LittleEndian.AppendUint32(data, uint32(len(keys)))
		var err error
		for _, key := range keys {
			data = appendString(data, key.String())
			data, err = appendCanonicalNBT(data, reflected.MapIndex(key).Interface())
			if err != nil {
				return nil, fmt.Errorf("%s: %w", key.String(), err)
			}
		}
		return data, nil
	case reflect.Slice, reflect.Array:
		data = append(data, 9)
		data = binary.LittleEndian.AppendUint32(data, uint32(reflected.Len()))
		var err error
		for index := 0; index < reflected.Len(); index++ {
			data, err = appendCanonicalNBT(data, reflected.Index(index).Interface())
			if err != nil {
				return nil, fmt.Errorf("[%d]: %w", index, err)
			}
		}
		return data, nil
	}
	return nil, fmt.Errorf("unsupported NBT type %T", value)
}

func canonicalNBT(value map[string]any) ([]byte, error) {
	return appendCanonicalNBT([]byte{'W', 'S', 'N', 'B', 1}, value)
}

func escapeJSONPointer(value string) string {
	return strings.ReplaceAll(strings.ReplaceAll(value, "~", "~0"), "/", "~1")
}

func appendCanonicalNBTField(fields *[]nbtFieldManifest, path string, value any) error {
	encoded, err := appendCanonicalNBT(nil, value)
	if err != nil {
		return err
	}
	if len(encoded) == 0 {
		return fmt.Errorf("empty canonical NBT field at %s", path)
	}
	*fields = append(*fields, nbtFieldManifest{Path: path, Type: encoded[0], Value: digest(encoded)})
	if encoded[0] != 9 && encoded[0] != 10 {
		return nil
	}

	reflected := reflect.ValueOf(value)
	for reflected.Kind() == reflect.Interface || reflected.Kind() == reflect.Pointer {
		if reflected.IsNil() {
			return fmt.Errorf("nil NBT value at %s", path)
		}
		reflected = reflected.Elem()
	}
	if encoded[0] == 10 {
		keys := reflected.MapKeys()
		sort.Slice(keys, func(i, j int) bool { return keys[i].String() < keys[j].String() })
		for _, key := range keys {
			childPath := path + "/" + escapeJSONPointer(key.String())
			if err := appendCanonicalNBTField(fields, childPath, reflected.MapIndex(key).Interface()); err != nil {
				return fmt.Errorf("%s: %w", childPath, err)
			}
		}
		return nil
	}
	for index := 0; index < reflected.Len(); index++ {
		childPath := path + "/" + strconv.Itoa(index)
		if err := appendCanonicalNBTField(fields, childPath, reflected.Index(index).Interface()); err != nil {
			return fmt.Errorf("%s: %w", childPath, err)
		}
	}
	return nil
}

func canonicalNBTFields(value map[string]any) ([]nbtFieldManifest, error) {
	fields := make([]nbtFieldManifest, 0, len(value))
	keys := make([]string, 0, len(value))
	for key := range value {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, key := range keys {
		path := "/" + escapeJSONPointer(key)
		if err := appendCanonicalNBTField(&fields, path, value[key]); err != nil {
			return nil, fmt.Errorf("%s: %w", path, err)
		}
	}
	return fields, nil
}

type detailRequest struct {
	chunkX int32
	chunkZ int32
	subY   int
	layer  uint8
}

func parseOptions(arguments []string) (*detailRequest, string, error) {
	var detail *detailRequest
	forcedFormat := ""
	for len(arguments) != 0 {
		switch arguments[0] {
		case "--format":
			if len(arguments) < 2 || forcedFormat != "" {
				return nil, "", fmt.Errorf("--format requires one unique format name")
			}
			forcedFormat = arguments[1]
			arguments = arguments[2:]
		case "--detail":
			if len(arguments) < 5 || detail != nil {
				return nil, "", fmt.Errorf("--detail requires chunkX chunkZ subY layer")
			}
			values := make([]int64, 4)
			for index := range values {
				parsed, err := strconv.ParseInt(arguments[index+1], 10, 32)
				if err != nil {
					return nil, "", fmt.Errorf("invalid detail argument %q: %w", arguments[index+1], err)
				}
				values[index] = parsed
			}
			if values[3] < 0 || values[3] > 1 {
				return nil, "", fmt.Errorf("detail layer must be 0 or 1")
			}
			detail = &detailRequest{
				chunkX: int32(values[0]), chunkZ: int32(values[1]),
				subY: int(values[2]), layer: uint8(values[3]),
			}
			arguments = arguments[5:]
		default:
			return nil, "", fmt.Errorf("unknown manifest option %q", arguments[0])
		}
	}
	return detail, forcedFormat, nil
}

func openStructureReader(file *os.File, forcedFormat string) (structure.Structure, error) {
	if forcedFormat == "" {
		reader, err := structure.StructureFromFile(file)
		if err != nil {
			return nil, err
		}
		return normalizeOracleReader(reader), nil
	}
	var reader structure.Structure
	switch forcedFormat {
	case "GangBanV1":
		reader = &structure.GangBanV1{}
	case "GangBanV2":
		return openGangBanV2Oracle(file)
	case "GangBanV3":
		reader = &structure.GangBanV3{}
	case "GangBanV4":
		reader = &structure.GangBanV4{}
	case "GangBanV5":
		reader = &structure.GangBanV5{}
	case "GangBanV6":
		reader = &structure.GangBanV6{}
	case "GangBanV7":
		reader = &structure.GangBanV7{}
	case "FuHongV1":
		reader = &structure.FuHongV1{}
	case "FuHongV2":
		reader = &structure.FuHongV2{}
	case "FuHongV3":
		reader = &structure.FuHongV3{}
	case "FuHongV4":
		reader = &structure.FuHongV4{}
	case "FuHongV5":
		reader = &structure.FuHongV5{}
	case "BDS":
		reader = &structure.BDS{}
	case "NexusNP":
		reader = &structure.NexusNP{}
	case "BCF":
		reader = &structure.BCF{}
	case "CovStructure":
		reader = &structure.CovStructure{}
	case "Construction":
		reader = &structure.Construction{}
	case "AxiomBP":
		reader = &structure.AxiomBP{}
	case "TIBI":
		reader = &structure.TIBI{}
	case "Schematic":
		reader = &structure.Schematic{}
	case "SchemV1":
		reader = &structure.SchemV1{}
	case "SchemV2":
		reader = &structure.SchemV2{}
	case "Litematic":
		reader = &structure.Litematic{}
	case "MCStructure":
		reader = &structure.MCStructure{}
	case "MCWorld":
		reader = &structure.MCWorld{}
	case "BDX":
		reader = &structure.BDX{}
	case "MCFunction":
		reader = &structure.MCFunction{}
	case "KBDX":
		reader = &structure.KBDX{}
	case "IBImport":
		reader = &structure.IBImport{}
	case "MianYangV1":
		reader = &structure.MianYangV1{}
	case "MianYangV2":
		reader = &structure.MianYangV2{}
	case "MianYangV3":
		reader = &structure.MianYangV3{}
	case "MianYangV4":
		reader = &structure.MianYangV4{}
	case "RunAway":
		reader = &structure.RunAway{}
	case "QingXuV1":
		reader = &structure.QingXuV1{}
	case "TimeBuilderV1":
		reader = &structure.TimeBuilderV1{}
	default:
		return nil, fmt.Errorf("unsupported forced format %q", forcedFormat)
	}
	if _, err := file.Seek(0, io.SeekStart); err != nil {
		return nil, err
	}
	if err := reader.FromFile(file); err != nil {
		return nil, err
	}
	return normalizeOracleReader(reader), nil
}

func openGangBanV2Oracle(file *os.File) (structure.Structure, error) {
	if _, err := file.Seek(0, io.SeekStart); err != nil {
		return nil, err
	}
	var entries []json.RawMessage
	decoder := json.NewDecoder(file)
	decoder.UseNumber()
	if err := decoder.Decode(&entries); err != nil {
		return nil, err
	}
	if len(entries) < 2 {
		return nil, fmt.Errorf("GangBanV2 requires blocks and a palette")
	}

	var existingRange struct {
		Start []int `json:"start"`
		End   []int `json:"end"`
	}
	if err := json.Unmarshal(entries[len(entries)-2], &existingRange); err == nil &&
		len(existingRange.Start) == 3 && len(existingRange.End) == 3 {
		if _, err := file.Seek(0, io.SeekStart); err != nil {
			return nil, err
		}
		reader := &structure.GangBanV2{}
		if err := reader.FromFile(file); err != nil {
			return nil, err
		}
		return reader, nil
	}

	minimum := [3]int{math.MaxInt, math.MaxInt, math.MaxInt}
	maximum := [3]int{math.MinInt, math.MinInt, math.MinInt}
	for index, raw := range entries[:len(entries)-1] {
		var blockEntry struct {
			Position []int `json:"p"`
		}
		if err := json.Unmarshal(raw, &blockEntry); err != nil || len(blockEntry.Position) != 3 {
			return nil, fmt.Errorf("GangBanV2 block %d has invalid position", index)
		}
		for axis := range 3 {
			minimum[axis] = min(minimum[axis], blockEntry.Position[axis])
			maximum[axis] = max(maximum[axis], blockEntry.Position[axis])
		}
	}
	if minimum[0] == math.MaxInt {
		return nil, fmt.Errorf("GangBanV2 has no blocks")
	}
	rangeEntry, err := json.Marshal(map[string]any{
		"start": []int{minimum[0], minimum[1], minimum[2]},
		"end":   []int{maximum[0], maximum[1], maximum[2]},
	})
	if err != nil {
		return nil, err
	}
	rewritten := make([]json.RawMessage, 0, len(entries)+1)
	rewritten = append(rewritten, entries[:len(entries)-1]...)
	rewritten = append(rewritten, rangeEntry, entries[len(entries)-1])
	temporary, err := os.CreateTemp("", "waterstructure-gangban-v2-*.json")
	if err != nil {
		return nil, err
	}
	temporaryPath := temporary.Name()
	defer func() {
		_ = temporary.Close()
		_ = os.Remove(temporaryPath)
	}()
	if err := json.NewEncoder(temporary).Encode(rewritten); err != nil {
		return nil, err
	}
	if _, err := temporary.Seek(0, io.SeekStart); err != nil {
		return nil, err
	}
	reader := &structure.GangBanV2{}
	if err := reader.FromFile(temporary); err != nil {
		return nil, err
	}
	return reader, nil
}

func normalizeOracleReader(reader structure.Structure) structure.Structure {
	// Fatalder records Schematic byte-array offsets immediately before the
	// four-byte NBT array length, while its streaming methods expect payload
	// offsets. Keep the source oracle untouched and correct only the exported
	// offsets used by this independent manifest tool.
	if schematic, ok := reader.(*structure.Schematic); ok {
		schematic.BlocksTagGzipOffset += 4
		schematic.DataTagGzipOffset += 4
	}
	return reader
}

func blockDetail(c interface {
	Block(uint8, int16, uint8, uint8) uint32
}, request detailRequest, versions map[string]int32, cache map[uint32][]byte) ([]map[string]any, error) {
	cells := make([]map[string]any, 0, 4096)
	for y := 0; y < 16; y++ {
		for z := 0; z < 16; z++ {
			for x := 0; x < 16; x++ {
				runtimeID := c.Block(uint8(x), int16(request.subY*16+y), uint8(z), request.layer)
				encoded, ok := cache[runtimeID]
				if !ok {
					var err error
					encoded, err = canonicalRuntimeBlock(runtimeID, versions)
					if err != nil {
						return nil, err
					}
					cache[runtimeID] = encoded
				}
				name, values, found := runtimeBlockState(runtimeID)
				if !found {
					return nil, fmt.Errorf("runtime ID %d has no block state", runtimeID)
				}
				properties, err := propertiesFromRuntime(values)
				if err != nil {
					return nil, err
				}
				key, err := mappingLookupKey(name, properties)
				if err != nil {
					return nil, err
				}
				version, found := versions[key]
				if !found {
					return nil, fmt.Errorf("block state version not found for %s", name)
				}
				sort.Slice(properties, func(i, j int) bool { return properties[i].Name < properties[j].Name })
				states := make([]map[string]any, 0, len(properties))
				for _, property := range properties {
					manifestType := map[byte]byte{1: 0, 2: 1, 3: 2, 4: 3, 8: 4}[property.Type]
					states = append(states, map[string]any{"name": property.Name, "type": manifestType, "value": fmt.Sprint(property.Value)})
				}
				index := (y*16+z)*16 + x
				cells = append(cells, map[string]any{
					"index": index, "x": int(request.chunkX)*16 + x, "y": request.subY*16 + y + 64,
					"z": int(request.chunkZ)*16 + z, "state_sha256": digest(encoded),
					"name": name, "version": version, "states": states,
				})
			}
		}
	}
	return cells, nil
}

func main() {
	if err := configureManifestMemory(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(3)
	}
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: go_manifest <input> [output.json [--format name] [--detail chunkX chunkZ subY layer]]")
		os.Exit(1)
	}
	outputPath := ""
	optionArguments := []string(nil)
	if len(os.Args) >= 3 {
		outputPath = os.Args[2]
		optionArguments = os.Args[3:]
	}
	detail, forcedFormat, err := parseOptions(optionArguments)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	inputHash, err := inputDigest(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(3)
	}
	result := manifest{
		Schema:             2,
		BlockHashAlgorithm: "canonical-block-state-v1",
		NBTHashAlgorithm:   "canonical-nbt-v1",
		InputSHA256:        inputHash,
	}
	versions, err := loadVersions()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(3)
	}
	file, cleanupInput, err := openStructureInput(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(3)
	}
	defer cleanupInput()
	defer file.Close()
	reader, err := openStructureReader(file, forcedFormat)
	var chunkSpool, entitySpool *jsonSpool
	includeArrays := false
	if err != nil {
		result.Error = map[string]any{"category": "parse", "message": err.Error()}
	} else {
		defer reader.Close()
		size := reader.GetSize()
		offset := reader.GetOffsetPos()
		result.Format = reader.Name()
		result.Size = []int{size.Width, size.Height, size.Length}
		result.Offset = []int32{offset[0], offset[1], offset[2]}
		if detail != nil {
			result.NonAir, err = reader.CountNonAirBlocks()
		}
		if err != nil {
			result.Error = map[string]any{"category": "count", "message": err.Error()}
		} else {
			// Keep only one decoded chunk (and one NBT map) alive at a time.  The
			// arrays themselves are spooled to disk; retaining them in result.Chunks
			// or result.BlockEntities was the source of multi-GB manifest runs.
			stateCache := make(map[uint32][]byte)
			chunkSpool, err = newJSONSpool("chunks")
			if err != nil {
				result.Error = map[string]any{"category": "spool", "message": err.Error()}
			} else {
				entitySpool, err = newJSONSpool("entities")
				if err != nil {
					result.Error = map[string]any{"category": "spool", "message": err.Error()}
				}
			}
			includeArrays = result.Error == nil
			startX, endX := 0, size.GetChunkXCount()
			startZ, endZ := 0, size.GetChunkZCount()
			if detail != nil {
				startX, endX = int(detail.chunkX), int(detail.chunkX)+1
				startZ, endZ = int(detail.chunkZ), int(detail.chunkZ)+1
				if startX < 0 || startX >= size.GetChunkXCount() || startZ < 0 || startZ >= size.GetChunkZCount() {
					result.Error = map[string]any{"category": "detail", "message": "detail chunk is outside structure bounds"}
				}
			}
			processedChunks := 0
			semanticNonAir := 0
			const manifestChunkBatch = 8
		chunkLoop:
			for x := startX; result.Error == nil && x < endX; x++ {
				for batchZ := startZ; batchZ < endZ; batchZ += manifestChunkBatch {
					batchEndZ := min(batchZ+manifestChunkBatch, endZ)
					positions := make([]define.ChunkPos, 0, batchEndZ-batchZ)
					for z := batchZ; z < batchEndZ; z++ {
						positions = append(positions, define.ChunkPos{int32(x), int32(z)})
					}
					chunks, chunkErr := reader.GetChunks(positions)
					if chunkErr != nil {
						result.Error = map[string]any{"category": "chunks", "message": chunkErr.Error()}
						break chunkLoop
					}
					var entities map[define.ChunkPos]map[define.BlockPos]map[string]any
					if detail == nil {
						entities, chunkErr = reader.GetChunksNBT(positions)
						if chunkErr != nil {
							result.Error = map[string]any{"category": "nbt", "message": chunkErr.Error()}
							break chunkLoop
						}
					}
					for _, pos := range positions {
						entry := chunkManifest{X: pos.X(), Z: pos.Z(), Subchunks: []subManifest{}}
						current := chunks[pos]
						if current != nil {
							if detail != nil && detail.chunkX == pos.X() && detail.chunkZ == pos.Z() {
								cells, detailErr := blockDetail(current, *detail, versions, stateCache)
								if detailErr != nil {
									result.Error = map[string]any{"category": "detail", "message": detailErr.Error()}
									break chunkLoop
								}
								result.Detail = cells
							}
							if detail == nil {
								for subY := -4; subY <= 19; subY++ {
									hash0, nonAir0, hashErr := layerDigest(current, subY, 0, versions, stateCache)
									if hashErr != nil {
										result.Error = map[string]any{"category": "canonical_block", "message": hashErr.Error()}
										break chunkLoop
									}
									hash1, nonAir1, hashErr := layerDigest(current, subY, 1, versions, stateCache)
									if hashErr != nil {
										result.Error = map[string]any{"category": "canonical_block", "message": hashErr.Error()}
										break chunkLoop
									}
									semanticNonAir += nonAir0 + nonAir1
									if nonAir0 != 0 || nonAir1 != 0 {
										entry.Subchunks = append(entry.Subchunks, subManifest{Y: subY, Layer0: hash0, Layer1: hash1})
									}
								}
							}
						}
						if err := chunkSpool.append(entry); err != nil {
							result.Error = map[string]any{"category": "spool", "message": err.Error()}
							break chunkLoop
						}

						if result.Error == nil && detail == nil {
							values := entities[pos]
							localEntities := make([]entityManifest, 0, len(values))
							for entityPos, value := range values {
								canonical, canonicalErr := canonicalNBT(value)
								if canonicalErr != nil {
									result.Error = map[string]any{"category": "canonical_nbt", "message": canonicalErr.Error()}
									break chunkLoop
								}
								fields, fieldsErr := canonicalNBTFields(value)
								if fieldsErr != nil {
									result.Error = map[string]any{"category": "canonical_nbt_fields", "message": fieldsErr.Error()}
									break chunkLoop
								}
								localEntities = append(localEntities, entityManifest{
									X: entityPos[0], Y: entityPos[1], Z: entityPos[2], Hash: digest(canonical), Fields: fields,
								})
							}
							sort.Slice(localEntities, func(i, j int) bool {
								a, b := localEntities[i], localEntities[j]
								if a.X != b.X {
									return a.X < b.X
								}
								if a.Y != b.Y {
									return a.Y < b.Y
								}
								if a.Z != b.Z {
									return a.Z < b.Z
								}
								return a.Hash < b.Hash
							})
							for _, entity := range localEntities {
								if err := entitySpool.append(entity); err != nil {
									result.Error = map[string]any{"category": "spool", "message": err.Error()}
									break chunkLoop
								}
							}
						}

						processedChunks++
						if processedChunks%64 == 0 {
							runtime.GC()
						}
					}
					// Drop the bounded batch before asking the reader for the next one.
					chunks = nil
					entities = nil
				}
			}
			if result.Error == nil && detail == nil {
				result.NonAir = semanticNonAir
			}
			if result.Error != nil {
				includeArrays = false
			}
		}
	}
	if chunkSpool != nil {
		defer chunkSpool.cleanup()
	}
	if entitySpool != nil {
		defer entitySpool.cleanup()
	}
	if err := writeManifest(outputPath, result, chunkSpool, entitySpool, includeArrays); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(3)
	}
	if result.Error != nil {
		os.Exit(2)
	}
}
