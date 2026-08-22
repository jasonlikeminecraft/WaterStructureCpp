package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
)

const maxReportedDifferences = 100

type scalar struct {
	present bool
	value   string
}

type manifestError struct {
	present      bool
	category     scalar
	offset       scalar
	commandIndex scalar
	coordinate   []scalar
}

type metadata struct {
	schema             scalar
	blockHashAlgorithm scalar
	nbtHashAlgorithm   scalar
	inputSHA256        scalar
	format             scalar
	size               []scalar
	offset             []scalar
	nonAirBlocks       scalar
	manifestError      manifestError
}

type subchunk struct {
	Y      int64  `json:"y"`
	Layer0 string `json:"layer0_sha256"`
	Layer1 string `json:"layer1_sha256"`
}

type chunk struct {
	X         int64      `json:"x"`
	Z         int64      `json:"z"`
	Subchunks []subchunk `json:"subchunks"`
}

type nbtField struct {
	Path  string `json:"path"`
	Type  scalarJSON
	Value string `json:"value_sha256"`
}

// scalarJSON retains the PowerShell comparator's scalar semantics: JSON
// strings compare as their contents and integer values compare as decimal text.
type scalarJSON struct {
	Value string
}

func (s *scalarJSON) UnmarshalJSON(data []byte) error {
	value, err := scalarFromJSON(data)
	if err != nil {
		return err
	}
	s.Value = value.value
	return nil
}

type entity struct {
	X      int64      `json:"x"`
	Y      int64      `json:"y"`
	Z      int64      `json:"z"`
	Hash   string     `json:"nbt_sha256"`
	Fields []nbtField `json:"nbt_fields"`
}

type blockMismatch struct {
	ChunkX int64 `json:"chunk_x"`
	ChunkZ int64 `json:"chunk_z"`
	SubY   int64 `json:"sub_y"`
	Layer  int   `json:"layer"`
}

type comparisonReport struct {
	Match              bool           `json:"match"`
	DifferenceCount    uint64         `json:"difference_count"`
	Differences        []string       `json:"differences,omitempty"`
	FirstBlockMismatch *blockMismatch `json:"first_block_mismatch,omitempty"`
}

type comparator struct {
	report comparisonReport
}

func (c *comparator) add(path, goValue, cppValue string) {
	c.report.DifferenceCount++
	if len(c.report.Differences) < maxReportedDifferences {
		c.report.Differences = append(c.report.Differences,
			fmt.Sprintf("%s: Go=%s C++=%s", path, goValue, cppValue))
	}
}

func (c *comparator) scalar(path string, goValue, cppValue scalar) {
	if goValue.value != cppValue.value {
		c.add(path, goValue.value, cppValue.value)
	}
}

func (c *comparator) array(path string, goValue, cppValue []scalar) {
	if len(goValue) != len(cppValue) {
		c.add(path+".length", strconv.Itoa(len(goValue)), strconv.Itoa(len(cppValue)))
		return
	}
	for index := range goValue {
		c.scalar(fmt.Sprintf("%s[%d]", path, index), goValue[index], cppValue[index])
	}
}

type streamStage uint8

const (
	stageChunks streamStage = iota
	stageEntities
	stageDone
)

// manifestStream holds only metadata and the current chunk/entity. json.Decoder
// buffers a small input window; it never materializes either top-level array.
type manifestStream struct {
	path   string
	file   *os.File
	dec    *json.Decoder
	meta   metadata
	stage  streamStage
	target streamStage

	hasChunks      bool
	hasEntities    bool
	previousChunk  *chunk
	previousEntity *entity
}

func openManifest(path string, target streamStage) (*manifestStream, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	stream := &manifestStream{
		path: path, file: file, dec: json.NewDecoder(file),
		stage: stageDone, target: target,
	}
	stream.dec.UseNumber()
	token, err := stream.dec.Token()
	if err != nil {
		file.Close()
		return nil, fmt.Errorf("%s: read root: %w", path, err)
	}
	if delimiter, ok := token.(json.Delim); !ok || delimiter != '{' {
		file.Close()
		return nil, fmt.Errorf("%s: manifest root is not an object", path)
	}
	if err := stream.scanForArray(); err != nil {
		file.Close()
		return nil, err
	}
	return stream, nil
}

func (m *manifestStream) Close() error { return m.file.Close() }

func (m *manifestStream) scanForArray() error {
	for m.dec.More() {
		nameToken, err := m.dec.Token()
		if err != nil {
			return fmt.Errorf("%s: read property name: %w", m.path, err)
		}
		name, ok := nameToken.(string)
		if !ok {
			return fmt.Errorf("%s: non-string property name", m.path)
		}
		switch name {
		case "chunks":
			if m.hasChunks {
				return fmt.Errorf("%s: duplicate chunks array", m.path)
			}
			if err := expectArray(m.dec, m.path, name); err != nil {
				return err
			}
			m.hasChunks = true
			if m.target == stageChunks {
				m.stage = stageChunks
				return nil
			}
			if err := skipArrayContents(m.dec); err != nil {
				return fmt.Errorf("%s: skip chunks: %w", m.path, err)
			}
		case "block_entities":
			if m.hasEntities {
				return fmt.Errorf("%s: duplicate block_entities array", m.path)
			}
			if err := expectArray(m.dec, m.path, name); err != nil {
				return err
			}
			m.hasEntities = true
			if m.target == stageEntities {
				m.stage = stageEntities
				return nil
			}
			if err := skipArrayContents(m.dec); err != nil {
				return fmt.Errorf("%s: skip block_entities: %w", m.path, err)
			}
		default:
			if err := m.readMetadata(name); err != nil {
				return fmt.Errorf("%s: property %s: %w", m.path, name, err)
			}
		}
	}
	if _, err := m.dec.Token(); err != nil {
		return fmt.Errorf("%s: close root: %w", m.path, err)
	}
	m.stage = stageDone
	return nil
}

// skipArrayContents is called after the opening '[' has been consumed. It
// walks each element as tokens, so skipping a multi-gigabyte manifest array
// does not turn it into a Go slice or require a temporary copy.
func skipArrayContents(decoder *json.Decoder) error {
	for decoder.More() {
		if err := skipValue(decoder); err != nil {
			return err
		}
	}
	_, err := decoder.Token()
	return err
}

func expectArray(decoder *json.Decoder, path, name string) error {
	token, err := decoder.Token()
	if err != nil {
		return fmt.Errorf("%s: read %s: %w", path, name, err)
	}
	if delimiter, ok := token.(json.Delim); !ok || delimiter != '[' {
		return fmt.Errorf("%s: %s is not an array", path, name)
	}
	return nil
}

func (m *manifestStream) readMetadata(name string) error {
	switch name {
	case "schema":
		return m.dec.Decode(&m.meta.schema)
	case "block_hash_algorithm":
		return m.dec.Decode(&m.meta.blockHashAlgorithm)
	case "nbt_hash_algorithm":
		return m.dec.Decode(&m.meta.nbtHashAlgorithm)
	case "input_sha256":
		return m.dec.Decode(&m.meta.inputSHA256)
	case "format":
		return m.dec.Decode(&m.meta.format)
	case "size":
		return m.dec.Decode(&m.meta.size)
	case "offset":
		return m.dec.Decode(&m.meta.offset)
	case "non_air_blocks":
		return m.dec.Decode(&m.meta.nonAirBlocks)
	case "error":
		var raw json.RawMessage
		if err := m.dec.Decode(&raw); err != nil {
			return err
		}
		if strings.TrimSpace(string(raw)) == "null" {
			return nil
		}
		var decoded struct {
			Category     scalar   `json:"category"`
			Offset       scalar   `json:"offset"`
			CommandIndex scalar   `json:"command_index"`
			Coordinate   []scalar `json:"coordinate"`
		}
		if err := json.Unmarshal(raw, &decoded); err != nil {
			return err
		}
		m.meta.manifestError = manifestError{
			present: true, category: decoded.Category, offset: decoded.Offset,
			commandIndex: decoded.CommandIndex, coordinate: decoded.Coordinate,
		}
		return nil
	default:
		return skipValue(m.dec)
	}
}

func (s *scalar) UnmarshalJSON(data []byte) error {
	value, err := scalarFromJSON(data)
	if err != nil {
		return err
	}
	*s = value
	return nil
}

func scalarFromJSON(data []byte) (scalar, error) {
	trimmed := strings.TrimSpace(string(data))
	if trimmed == "null" {
		return scalar{present: true}, nil
	}
	var stringValue string
	if len(trimmed) != 0 && trimmed[0] == '"' {
		if err := json.Unmarshal(data, &stringValue); err != nil {
			return scalar{}, err
		}
		return scalar{present: true, value: stringValue}, nil
	}
	if trimmed == "true" || trimmed == "false" {
		return scalar{present: true, value: trimmed}, nil
	}
	if _, err := strconv.ParseFloat(trimmed, 64); err == nil {
		return scalar{present: true, value: normalizeNumber(trimmed)}, nil
	}
	return scalar{}, fmt.Errorf("expected scalar, got %s", trimmed)
}

func normalizeNumber(value string) string {
	if !strings.ContainsAny(value, ".eE") {
		return strings.TrimPrefix(value, "+")
	}
	parsed, err := strconv.ParseFloat(value, 64)
	if err != nil {
		return value
	}
	return strconv.FormatFloat(parsed, 'g', -1, 64)
}

func skipValue(decoder *json.Decoder) error {
	token, err := decoder.Token()
	if err != nil {
		return err
	}
	delimiter, compound := token.(json.Delim)
	if !compound || (delimiter != '{' && delimiter != '[') {
		return nil
	}
	for decoder.More() {
		if delimiter == '{' {
			if _, err := decoder.Token(); err != nil {
				return err
			}
		}
		if err := skipValue(decoder); err != nil {
			return err
		}
	}
	_, err = decoder.Token()
	return err
}

func (m *manifestStream) nextChunk() (*chunk, error) {
	if m.stage != stageChunks {
		return nil, nil
	}
	if !m.dec.More() {
		if _, err := m.dec.Token(); err != nil {
			return nil, fmt.Errorf("%s: close chunks: %w", m.path, err)
		}
		m.stage = stageDone
		return nil, nil
	}
	var value chunk
	if err := m.dec.Decode(&value); err != nil {
		return nil, fmt.Errorf("%s: decode chunk: %w", m.path, err)
	}
	if m.previousChunk != nil && compareChunkKey(*m.previousChunk, value) >= 0 {
		return nil, fmt.Errorf("%s: chunks are not strictly ordered at (%d,%d)", m.path, value.X, value.Z)
	}
	m.previousChunk = &chunk{X: value.X, Z: value.Z}
	return &value, nil
}

func (m *manifestStream) nextEntity() (*entity, error) {
	if m.stage != stageEntities {
		return nil, nil
	}
	if !m.dec.More() {
		if _, err := m.dec.Token(); err != nil {
			return nil, fmt.Errorf("%s: close block_entities: %w", m.path, err)
		}
		m.stage = stageDone
		return nil, nil
	}
	var value entity
	if err := m.dec.Decode(&value); err != nil {
		return nil, fmt.Errorf("%s: decode block entity: %w", m.path, err)
	}
	// Readers emit NBT in requested-chunk batches, but some formats expose
	// structure-local entity coordinates. Those coordinates can restart at
	// zero for the next source chunk, so they cannot prove global ordering.
	// The ordered merge below still compares one record from each manifest at
	// a time and reports any differing sequence without materializing the
	// complete entity array.
	m.previousEntity = &entity{X: value.X, Y: value.Y, Z: value.Z, Hash: value.Hash}
	return &value, nil
}

func compareChunkKey(left, right chunk) int {
	if left.X != right.X {
		if left.X < right.X {
			return -1
		}
		return 1
	}
	if left.Z < right.Z {
		return -1
	}
	if left.Z > right.Z {
		return 1
	}
	return 0
}

func floorDiv16(value int64) int64 {
	if value >= 0 {
		return value / 16
	}
	return -((-value + 15) / 16)
}

func compareEntityOrder(left, right entity) int {
	leftValues := [...]int64{floorDiv16(left.X), floorDiv16(left.Z), left.X, left.Y, left.Z}
	rightValues := [...]int64{floorDiv16(right.X), floorDiv16(right.Z), right.X, right.Y, right.Z}
	for index := range leftValues {
		if leftValues[index] < rightValues[index] {
			return -1
		}
		if leftValues[index] > rightValues[index] {
			return 1
		}
	}
	return strings.Compare(left.Hash, right.Hash)
}

func compareEntityCoordinate(left, right entity) int {
	leftValues := [...]int64{floorDiv16(left.X), floorDiv16(left.Z), left.X, left.Y, left.Z}
	rightValues := [...]int64{floorDiv16(right.X), floorDiv16(right.Z), right.X, right.Y, right.Z}
	for index := range leftValues {
		if leftValues[index] < rightValues[index] {
			return -1
		}
		if leftValues[index] > rightValues[index] {
			return 1
		}
	}
	return 0
}

func compareMetadata(c *comparator, goMeta, cppMeta metadata, ignoreInputSHA bool) bool {
	c.scalar("schema", goMeta.schema, cppMeta.schema)
	c.scalar("block_hash_algorithm", goMeta.blockHashAlgorithm, cppMeta.blockHashAlgorithm)
	c.scalar("nbt_hash_algorithm", goMeta.nbtHashAlgorithm, cppMeta.nbtHashAlgorithm)
	if !ignoreInputSHA {
		c.scalar("input_sha256", goMeta.inputSHA256, cppMeta.inputSHA256)
	}
	if goMeta.manifestError.present || cppMeta.manifestError.present {
		if goMeta.manifestError.present != cppMeta.manifestError.present {
			c.add("error.presence", strconv.FormatBool(goMeta.manifestError.present), strconv.FormatBool(cppMeta.manifestError.present))
		} else {
			c.scalar("error.category", goMeta.manifestError.category, cppMeta.manifestError.category)
			c.scalar("error.offset", goMeta.manifestError.offset, cppMeta.manifestError.offset)
			c.scalar("error.command_index", goMeta.manifestError.commandIndex, cppMeta.manifestError.commandIndex)
			c.array("error.coordinate", goMeta.manifestError.coordinate, cppMeta.manifestError.coordinate)
		}
		return false
	}
	c.scalar("format", goMeta.format, cppMeta.format)
	c.array("size", goMeta.size, cppMeta.size)
	c.array("offset", goMeta.offset, cppMeta.offset)
	c.scalar("non_air_blocks", goMeta.nonAirBlocks, cppMeta.nonAirBlocks)
	return true
}

func compareSubchunks(c *comparator, key string, goChunk, cppChunk chunk) {
	goByY := make(map[int64]subchunk, len(goChunk.Subchunks))
	cppByY := make(map[int64]subchunk, len(cppChunk.Subchunks))
	for _, value := range goChunk.Subchunks {
		goByY[value.Y] = value
	}
	for _, value := range cppChunk.Subchunks {
		cppByY[value.Y] = value
	}
	ys := make([]int64, 0, len(goByY)+len(cppByY))
	seen := make(map[int64]struct{}, len(goByY)+len(cppByY))
	for y := range goByY {
		seen[y] = struct{}{}
		ys = append(ys, y)
	}
	for y := range cppByY {
		if _, exists := seen[y]; !exists {
			ys = append(ys, y)
		}
	}
	sort.Slice(ys, func(i, j int) bool { return ys[i] < ys[j] })
	for _, y := range ys {
		goValue, goOK := goByY[y]
		cppValue, cppOK := cppByY[y]
		path := fmt.Sprintf("chunks[%s].subchunks[y=%d]", key, y)
		if !goOK {
			c.add(path+".presence", "false", "true")
			continue
		}
		if !cppOK {
			c.add(path+".presence", "true", "false")
			continue
		}
		if goValue.Layer0 != cppValue.Layer0 {
			if c.report.FirstBlockMismatch == nil {
				c.report.FirstBlockMismatch = &blockMismatch{ChunkX: goChunk.X, ChunkZ: goChunk.Z, SubY: y, Layer: 0}
			}
			c.add(path+".layer0_sha256", goValue.Layer0, cppValue.Layer0)
		}
		if goValue.Layer1 != cppValue.Layer1 {
			if c.report.FirstBlockMismatch == nil {
				c.report.FirstBlockMismatch = &blockMismatch{ChunkX: goChunk.X, ChunkZ: goChunk.Z, SubY: y, Layer: 1}
			}
			c.add(path+".layer1_sha256", goValue.Layer1, cppValue.Layer1)
		}
	}
}

func compareChunks(c *comparator, goStream, cppStream *manifestStream) error {
	goChunk, err := goStream.nextChunk()
	if err != nil {
		return err
	}
	cppChunk, err := cppStream.nextChunk()
	if err != nil {
		return err
	}
	for goChunk != nil || cppChunk != nil {
		if goChunk == nil {
			c.add(fmt.Sprintf("chunks[%d,%d].presence", cppChunk.X, cppChunk.Z), "false", "true")
			cppChunk, err = cppStream.nextChunk()
		} else if cppChunk == nil {
			c.add(fmt.Sprintf("chunks[%d,%d].presence", goChunk.X, goChunk.Z), "true", "false")
			goChunk, err = goStream.nextChunk()
		} else {
			order := compareChunkKey(*goChunk, *cppChunk)
			if order < 0 {
				c.add(fmt.Sprintf("chunks[%d,%d].presence", goChunk.X, goChunk.Z), "true", "false")
				goChunk, err = goStream.nextChunk()
			} else if order > 0 {
				c.add(fmt.Sprintf("chunks[%d,%d].presence", cppChunk.X, cppChunk.Z), "false", "true")
				cppChunk, err = cppStream.nextChunk()
			} else {
				key := fmt.Sprintf("%d,%d", goChunk.X, goChunk.Z)
				compareSubchunks(c, key, *goChunk, *cppChunk)
				goChunk, err = goStream.nextChunk()
				if err == nil {
					cppChunk, err = cppStream.nextChunk()
				}
			}
		}
		if err != nil {
			return err
		}
	}
	return nil
}

func compareFields(c *comparator, path string, goFields, cppFields []nbtField) {
	goByPath := make(map[string]nbtField, len(goFields))
	cppByPath := make(map[string]nbtField, len(cppFields))
	for _, field := range goFields {
		goByPath[field.Path] = field
	}
	for _, field := range cppFields {
		cppByPath[field.Path] = field
	}
	paths := make([]string, 0, len(goByPath)+len(cppByPath))
	seen := make(map[string]struct{}, len(goByPath)+len(cppByPath))
	for fieldPath := range goByPath {
		seen[fieldPath] = struct{}{}
		paths = append(paths, fieldPath)
	}
	for fieldPath := range cppByPath {
		if _, exists := seen[fieldPath]; !exists {
			paths = append(paths, fieldPath)
		}
	}
	sort.Strings(paths)
	for _, fieldPath := range paths {
		goField, goOK := goByPath[fieldPath]
		cppField, cppOK := cppByPath[fieldPath]
		current := path + ".nbt" + fieldPath
		if !goOK {
			c.add(current+".presence", "false", "true")
			continue
		}
		if !cppOK {
			c.add(current+".presence", "true", "false")
			continue
		}
		if goField.Type.Value != cppField.Type.Value {
			c.add(current+".type", goField.Type.Value, cppField.Type.Value)
		}
		if goField.Value != cppField.Value {
			c.add(current+".value_sha256", goField.Value, cppField.Value)
		}
	}
}

func compareEntities(c *comparator, goStream, cppStream *manifestStream) error {
	goEntity, err := goStream.nextEntity()
	if err != nil {
		return err
	}
	cppEntity, err := cppStream.nextEntity()
	if err != nil {
		return err
	}
	var goCount, cppCount uint64
	for goEntity != nil || cppEntity != nil {
		if goEntity == nil {
			cppCount++
			c.add(fmt.Sprintf("block_entities[x=%d,y=%d,z=%d].presence", cppEntity.X, cppEntity.Y, cppEntity.Z), "false", "true")
			cppEntity, err = cppStream.nextEntity()
		} else if cppEntity == nil {
			goCount++
			c.add(fmt.Sprintf("block_entities[x=%d,y=%d,z=%d].presence", goEntity.X, goEntity.Y, goEntity.Z), "true", "false")
			goEntity, err = goStream.nextEntity()
		} else {
			order := compareEntityCoordinate(*goEntity, *cppEntity)
			if order < 0 {
				goCount++
				c.add(fmt.Sprintf("block_entities[x=%d,y=%d,z=%d].presence", goEntity.X, goEntity.Y, goEntity.Z), "true", "false")
				goEntity, err = goStream.nextEntity()
			} else if order > 0 {
				cppCount++
				c.add(fmt.Sprintf("block_entities[x=%d,y=%d,z=%d].presence", cppEntity.X, cppEntity.Y, cppEntity.Z), "false", "true")
				cppEntity, err = cppStream.nextEntity()
			} else {
				goCount++
				cppCount++
				path := fmt.Sprintf("block_entities[x=%d,y=%d,z=%d]", goEntity.X, goEntity.Y, goEntity.Z)
				if goEntity.Hash != cppEntity.Hash {
					c.add(path+".nbt_sha256", goEntity.Hash, cppEntity.Hash)
					compareFields(c, path, goEntity.Fields, cppEntity.Fields)
				}
				goEntity, err = goStream.nextEntity()
				if err == nil {
					cppEntity, err = cppStream.nextEntity()
				}
			}
		}
		if err != nil {
			return err
		}
	}
	if goCount != cppCount {
		c.add("block_entities.length", strconv.FormatUint(goCount, 10), strconv.FormatUint(cppCount, 10))
	}
	return nil
}

func comparePaths(goPath, cppPath string) (comparisonReport, error) {
	return comparePathsWithOptions(goPath, cppPath, false)
}

func comparePathsWithOptions(goPath, cppPath string, ignoreInputSHA bool) (comparisonReport, error) {
	// Metadata and the two unbounded arrays are read in independent passes.
	// This makes comparison independent of JSON object-key order while keeping
	// memory bounded: each pass holds at most one chunk or block entity.
	goMetadata, err := openManifest(goPath, stageDone)
	if err != nil {
		return comparisonReport{}, err
	}
	cppMetadata, err := openManifest(cppPath, stageDone)
	if err != nil {
		goMetadata.Close()
		return comparisonReport{}, err
	}

	comparison := &comparator{}
	compareArrays := compareMetadata(comparison, goMetadata.meta, cppMetadata.meta, ignoreInputSHA)
	if err := goMetadata.Close(); err != nil {
		cppMetadata.Close()
		return comparisonReport{}, err
	}
	if err := cppMetadata.Close(); err != nil {
		return comparisonReport{}, err
	}
	if compareArrays {
		goChunks, err := openManifest(goPath, stageChunks)
		if err != nil {
			return comparisonReport{}, err
		}
		cppChunks, err := openManifest(cppPath, stageChunks)
		if err != nil {
			goChunks.Close()
			return comparisonReport{}, err
		}
		if err := compareChunks(comparison, goChunks, cppChunks); err != nil {
			goChunks.Close()
			cppChunks.Close()
			return comparisonReport{}, err
		}
		if err := goChunks.Close(); err != nil {
			cppChunks.Close()
			return comparisonReport{}, err
		}
		if err := cppChunks.Close(); err != nil {
			return comparisonReport{}, err
		}

		goEntities, err := openManifest(goPath, stageEntities)
		if err != nil {
			return comparisonReport{}, err
		}
		cppEntities, err := openManifest(cppPath, stageEntities)
		if err != nil {
			goEntities.Close()
			return comparisonReport{}, err
		}
		if err := compareEntities(comparison, goEntities, cppEntities); err != nil {
			goEntities.Close()
			cppEntities.Close()
			return comparisonReport{}, err
		}
		if err := goEntities.Close(); err != nil {
			cppEntities.Close()
			return comparisonReport{}, err
		}
		if err := cppEntities.Close(); err != nil {
			return comparisonReport{}, err
		}
	}
	comparison.report.Match = comparison.report.DifferenceCount == 0
	return comparison.report, nil
}

func writeReport(path string, report comparisonReport) error {
	if path == "" {
		return nil
	}
	file, err := os.OpenFile(path, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	encoder := json.NewEncoder(file)
	err = encoder.Encode(report)
	if closeErr := file.Close(); err == nil {
		err = closeErr
	}
	return err
}

func main() {
	goPath := flag.String("go", "", "Go manifest JSON path")
	cppPath := flag.String("cpp", "", "C++ manifest JSON path")
	reportPath := flag.String("report", "", "optional compact JSON report path")
	ignoreInputSHA := flag.Bool("ignore-input-sha", false,
		"compare semantic manifest content from different input files")
	flag.Parse()
	if *goPath == "" || *cppPath == "" || flag.NArg() != 0 {
		fmt.Fprintln(os.Stderr, "usage: stream_manifest_diff -go go.json -cpp cpp.json [-report report.json] [-ignore-input-sha]")
		os.Exit(2)
	}
	report, err := comparePathsWithOptions(*goPath, *cppPath, *ignoreInputSHA)
	if err != nil {
		fmt.Fprintln(os.Stderr, "stream manifest diff:", err)
		os.Exit(2)
	}
	if err := writeReport(*reportPath, report); err != nil {
		fmt.Fprintln(os.Stderr, "stream manifest diff: write report:", err)
		os.Exit(2)
	}
	if report.Match {
		fmt.Printf("manifest match: %s == %s\n", *goPath, *cppPath)
		return
	}
	fmt.Fprintf(os.Stderr, "manifest mismatch (%d differences)\n", report.DifferenceCount)
	for _, difference := range report.Differences {
		fmt.Fprintln(os.Stderr, difference)
	}
	os.Exit(1)
}
