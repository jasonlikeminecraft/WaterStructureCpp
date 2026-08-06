package main

import (
	"bytes"
	"compress/gzip"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"sort"
	"strconv"
	"strings"

	bedrockblock "github.com/TriM-Organization/bedrock-world-operator/block"
	bedrockdefine "github.com/TriM-Organization/bedrock-world-operator/define"
	"github.com/Yeah114/blocks"
	"github.com/Yeah114/blocks/convertor"
	"github.com/Yeah114/blocks/describe"
	"github.com/andybalholm/brotli"
	"github.com/sandertv/gophertunnel/minecraft/nbt"
)

const sourceVersion = "github.com/Yeah114/blocks@v0.0.0-20251025181709-54ab0b294dfe + bedrock-world-operator@v1.4.0"

type property struct {
	Type  string `json:"type"`
	Value string `json:"value"`
}

type state struct {
	Name       string              `json:"name"`
	States     map[string]property `json:"states,omitempty"`
	Version    uint32              `json:"version"`
	SourceRTID uint32              `json:"source_runtime_id"`
}

type legacyEntry struct {
	Name string `json:"name"`
	Aux  uint16 `json:"aux"`
}

type legacyPaletteEntry struct {
	Name string `json:"name"`
	Data uint16 `json:"data"`
}

type legacyPalette struct {
	Blocks []legacyPaletteEntry `json:"blocks"`
}

type outputFile struct {
	Schema               uint32                       `json:"schema"`
	Source               string                       `json:"source"`
	Palette              []state                      `json:"palette"`
	SchematicIndices     []uint32                     `json:"schematic_indices"`
	BDX117Indices        []uint32                     `json:"bdx_117_indices"`
	LegacyToRuntime      map[string]map[uint16]uint32 `json:"legacy_to_runtime,omitempty"`
	LegacyDefaults       map[string]uint32            `json:"legacy_default_runtime,omitempty"`
	LegacyStateToRuntime map[string]map[string]uint32 `json:"legacy_state_to_runtime,omitempty"`
	LegacyStateOrder     map[string]map[string]uint32 `json:"legacy_state_order,omitempty"`
	JavaToRuntime        map[string]uint32            `json:"java_to_runtime"`
	JavaDefaults         map[string]uint32            `json:"java_default_runtime,omitempty"`
}

func typedProperty(value any) (property, error) {
	switch value := value.(type) {
	case bool:
		if value {
			return property{Type: "byte", Value: "1"}, nil
		}
		return property{Type: "byte", Value: "0"}, nil
	case int8:
		return property{Type: "byte", Value: strconv.FormatInt(int64(value), 10)}, nil
	case uint8:
		return property{Type: "byte", Value: strconv.FormatUint(uint64(value), 10)}, nil
	case int16:
		return property{Type: "short", Value: strconv.FormatInt(int64(value), 10)}, nil
	case uint16:
		return property{Type: "short", Value: strconv.FormatUint(uint64(value), 10)}, nil
	case int32:
		return property{Type: "int", Value: strconv.FormatInt(int64(value), 10)}, nil
	case uint32:
		return property{Type: "int", Value: strconv.FormatUint(uint64(value), 10)}, nil
	case int:
		return property{Type: "int", Value: strconv.Itoa(value)}, nil
	case int64:
		return property{Type: "long", Value: strconv.FormatInt(value, 10)}, nil
	case uint64:
		return property{Type: "long", Value: strconv.FormatUint(value, 10)}, nil
	case string:
		return property{Type: "string", Value: value}, nil
	default:
		return property{}, fmt.Errorf("unsupported state type %T", value)
	}
}

func stateFor(runtimeID uint32) (state, error) {
	name, properties, found := blocks.RuntimeIDToState(runtimeID)
	if !found {
		return state{}, fmt.Errorf("runtime ID %d is missing", runtimeID)
	}
	if !strings.Contains(name, ":") {
		name = "minecraft:" + name
	}
	bedrockRuntimeID, found := bedrockblock.StateToRuntimeID(name, properties)
	if !found {
		bedrockRuntimeID, found = bedrockblock.StateToRuntimeID("minecraft:unknown", nil)
		if !found {
			return state{}, fmt.Errorf("runtime ID %d cannot resolve to Bedrock runtime state", runtimeID)
		}
	}
	name, properties, found = bedrockblock.RuntimeIDToState(bedrockRuntimeID)
	if !found {
		return state{}, fmt.Errorf("Bedrock runtime ID %d is missing", bedrockRuntimeID)
	}
	result := state{
		Name:       name,
		States:     make(map[string]property, len(properties)),
		Version:    blocks.NEMC_BLOCK_VERSION,
		SourceRTID: runtimeID,
	}
	keys := make([]string, 0, len(properties))
	for key := range properties {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, key := range keys {
		value, err := typedProperty(properties[key])
		if err != nil {
			return state{}, fmt.Errorf("runtime ID %d property %s: %w", runtimeID, key, err)
		}
		result.States[key] = value
	}
	return result, nil
}

func addLegacyAlias(aliases map[string]map[string]struct{}, name string) {
	if !strings.Contains(name, ":") {
		name = "minecraft:" + name
	}
	canonical := strings.ToLower(name)
	if aliases[canonical] == nil {
		aliases[canonical] = make(map[string]struct{})
	}
	aliases[canonical][name] = struct{}{}
}

func main() {
	outputPath := flag.String("output", "block_mappings_v1.json", "output JSON path")
	bdxPath := flag.String("bdx", "", "bdx_runtimeIds_117.json input path")
	legacyPalettePath := flag.String("legacy-palette", "", "legacy_block_palette JSON input path")
	bedrockRecordsPath := flag.String("bedrock-records", "", "blocks bedrock_java_to_translate.br input path")
	specificRecordsPath := flag.String("specific-records", "", "blocks specific_legacy_value_to_translate.br input path")
	bedrockStatesPath := flag.String("bedrock-states", "", "BedrockWorldOperator netease_block_states.nbt input path")
	flag.Parse()

	palette := make([]state, len(blocks.MC_CURRENT.Blocks()))
	javaToRuntime := make(map[string]uint32)
	legacyNames := make(map[string]map[string]struct{})
	for runtimeID := range palette {
		rawName, _, found := blocks.RuntimeIDToState(uint32(runtimeID))
		if !found {
			panic(fmt.Sprintf("raw runtime ID %d is missing", runtimeID))
		}
		if !strings.Contains(rawName, ":") {
			rawName = "minecraft:" + rawName
		}
		addLegacyAlias(legacyNames, rawName)
		converted, err := stateFor(uint32(runtimeID))
		if err != nil {
			panic(err)
		}
		palette[runtimeID] = converted
		if javaState, found := blocks.RuntimeIDToJavaBlockStr(uint32(runtimeID)); found {
			if resolved, ok := blocks.JavaBlockStrToRuntimeID(javaState); ok {
				javaToRuntime[javaState] = resolved
			}
		}
	}
	javaToRuntime["minecraft:air"] = blocks.AIR_RUNTIMEID
	javaToRuntime["minecraft:cave_air"] = blocks.AIR_RUNTIMEID
	javaToRuntime["minecraft:void_air"] = blocks.AIR_RUNTIMEID

	schematic := blocks.GetSchematicMapping()
	schematicIndices := make([]uint32, 0, 256*256)
	for blockID := range schematic {
		for data := range schematic[blockID] {
			schematicIndices = append(schematicIndices, schematic[blockID][data])
		}
	}

	var bdxIndices []uint32
	if *bdxPath != "" {
		data, err := os.ReadFile(*bdxPath)
		if err != nil {
			panic(err)
		}
		var legacy []legacyEntry
		var raw [][2]any
		if err := json.Unmarshal(data, &raw); err != nil {
			panic(err)
		}
		legacy = make([]legacyEntry, 0, len(raw))
		for _, item := range raw {
			name, nameOK := item[0].(string)
			auxValue, auxOK := item[1].(float64)
			if !nameOK || !auxOK {
				panic("invalid BDX legacy entry")
			}
			legacy = append(legacy, legacyEntry{Name: name, Aux: uint16(auxValue)})
		}
		bdxIndices = make([]uint32, 0, len(legacy))
		for _, entry := range legacy {
			runtimeID, found := blocks.LegacyBlockToRuntimeID(entry.Name, entry.Aux)
			if !found {
				runtimeID = uint32(blocks.MC_CURRENT.UnknownRitd())
			}
			bdxIndices = append(bdxIndices, runtimeID)
		}
	}

	legacyToRuntime := make(map[string]map[uint16]uint32)
	legacyDefaults := make(map[string]uint32)
	javaDefaults := make(map[string]uint32)
	legacyStateToRuntime := make(map[string]map[string]uint32)
	legacyStateOrder := make(map[string]map[string]uint32)
	var stateRecordOrder uint32
	for _, block := range blocks.MC_CURRENT.Blocks() {
		name := block.ShortName()
		if !strings.Contains(name, ":") {
			name = "minecraft:" + name
		}
		canonicalName := strings.ToLower(name)
		if legacyStateToRuntime[canonicalName] == nil {
			legacyStateToRuntime[canonicalName] = make(map[string]uint32)
			legacyStateOrder[canonicalName] = make(map[string]uint32)
		}
		properties := block.StatesForSearch()
		stateKey := properties.InPreciseSNBT()
		legacyStateToRuntime[canonicalName][stateKey] = block.Rtid()
		if _, exists := legacyStateOrder[canonicalName][stateKey]; !exists {
			legacyStateOrder[canonicalName][stateKey] = stateRecordOrder
		}
		stateRecordOrder++
	}
	var maximumLegacyData uint16
	if *legacyPalettePath != "" {
		data, err := os.ReadFile(*legacyPalettePath)
		if err != nil {
			panic(err)
		}
		var palette legacyPalette
		if err := json.Unmarshal(data, &palette); err != nil {
			panic(err)
		}
		for _, entry := range palette.Blocks {
			name := entry.Name
			if !strings.Contains(name, ":") {
				name = "minecraft:" + name
			}
			addLegacyAlias(legacyNames, name)
			if entry.Data > maximumLegacyData {
				maximumLegacyData = entry.Data
			}
		}
	}
	for _, recordsPath := range []string{*bedrockRecordsPath, *specificRecordsPath} {
		if recordsPath == "" {
			continue
		}
		compressed, err := os.ReadFile(recordsPath)
		if err != nil {
			panic(err)
		}
		decoded, err := io.ReadAll(brotli.NewReader(bytes.NewReader(compressed)))
		if err != nil {
			panic(err)
		}
		records, err := convertor.ReadRecordsFromString(string(decoded))
		if err != nil {
			panic(err)
		}
		for _, record := range records {
			name := record.Name
			if !strings.Contains(name, ":") {
				name = "minecraft:" + name
			}
			addLegacyAlias(legacyNames, name)
			if value, ok := record.GetLegacyValue(); ok {
				if value > maximumLegacyData {
					maximumLegacyData = value
				}
				continue
			}
			properties, parseErr := describe.PropsForSearchFromStr(record.SNBTStateOrValue)
			if parseErr != nil {
				panic(parseErr)
			}
			runtimeID, found := blocks.BlockNameAndStateStrToRuntimeID(name, record.SNBTStateOrValue)
			if !found {
				continue
			}
			canonicalName := strings.ToLower(name)
			if legacyStateToRuntime[canonicalName] == nil {
				legacyStateToRuntime[canonicalName] = make(map[string]uint32)
				legacyStateOrder[canonicalName] = make(map[string]uint32)
			}
			stateKey := properties.InPreciseSNBT()
			legacyStateToRuntime[canonicalName][stateKey] = runtimeID
			if _, exists := legacyStateOrder[canonicalName][stateKey]; !exists {
				legacyStateOrder[canonicalName][stateKey] = stateRecordOrder
			}
			stateRecordOrder++
		}
	}
	if *bedrockStatesPath != "" {
		compressed, err := os.Open(*bedrockStatesPath)
		if err != nil {
			panic(err)
		}
		gzipReader, err := gzip.NewReader(compressed)
		if err != nil {
			compressed.Close()
			panic(err)
		}
		decoded, err := io.ReadAll(gzipReader)
		closeGzipErr := gzipReader.Close()
		closeFileErr := compressed.Close()
		if err != nil {
			panic(err)
		}
		if closeGzipErr != nil {
			panic(closeGzipErr)
		}
		if closeFileErr != nil {
			panic(closeFileErr)
		}
		var palette struct {
			Blocks []bedrockdefine.NetEaseBlock `nbt:"blocks"`
		}
		if err := nbt.NewDecoderWithEncoding(bytes.NewReader(decoded), nbt.BigEndian).Decode(&palette); err != nil {
			panic(err)
		}
		for _, blockState := range palette.Blocks {
			name := blockState.Name
			if !strings.Contains(name, ":") {
				name = "minecraft:" + name
			}
			addLegacyAlias(legacyNames, name)
		}
	}
	legacyNameList := make([]string, 0, len(legacyNames))
	for name := range legacyNames {
		legacyNameList = append(legacyNameList, name)
	}
	sort.Strings(legacyNameList)
	for _, name := range legacyNameList {
		if runtimeID, found := blocks.BlockStrToRuntimeID(name); found {
			javaDefaults[name] = runtimeID
		}
	}
	for _, name := range legacyNameList {
		aliases := make([]string, 0, len(legacyNames[name]))
		for alias := range legacyNames[name] {
			aliases = append(aliases, alias)
		}
		sort.Slice(aliases, func(i, j int) bool {
			if aliases[i] == name {
				return true
			}
			if aliases[j] == name {
				return false
			}
			return aliases[i] < aliases[j]
		})
		// 65535 overflows a uint16 + 1 inside the upstream fuzzy matcher.
		// 65534 is beyond every known legacy metadata table and reaches the
		// same first-state fallback without triggering that upstream panic.
		var defaultRuntimeID uint32
		hasDefault := false
		values := make(map[uint16]uint32)
		for _, alias := range aliases {
			aliasDefault, found := blocks.LegacyBlockToRuntimeID(alias, uint16(65534))
			if !found {
				continue
			}
			if !hasDefault {
				defaultRuntimeID = aliasDefault
				hasDefault = true
			}
			for data := uint32(0); data <= uint32(maximumLegacyData); data++ {
				runtimeID, found := blocks.LegacyBlockToRuntimeID(alias, uint16(data))
				if !found || runtimeID == aliasDefault {
					continue
				}
				key := uint16(data)
				if _, exists := values[key]; exists {
					continue
				}
				values[key] = runtimeID
			}
		}
		if !hasDefault {
			continue
		}
		legacyDefaults[name] = defaultRuntimeID
		if len(values) != 0 {
			legacyToRuntime[name] = values
		}
	}

	result := outputFile{
		Schema:               1,
		Source:               sourceVersion,
		Palette:              palette,
		SchematicIndices:     schematicIndices,
		BDX117Indices:        bdxIndices,
		LegacyToRuntime:      legacyToRuntime,
		LegacyDefaults:       legacyDefaults,
		LegacyStateToRuntime: legacyStateToRuntime,
		LegacyStateOrder:     legacyStateOrder,
		JavaToRuntime:        javaToRuntime,
		JavaDefaults:         javaDefaults,
	}
	file, err := os.Create(*outputPath)
	if err != nil {
		panic(err)
	}
	defer file.Close()
	encoder := json.NewEncoder(file)
	encoder.SetEscapeHTML(false)
	if err := encoder.Encode(result); err != nil {
		panic(err)
	}
	fmt.Printf("palette=%d schematic=%d bdx117=%d legacy_names=%d legacy_defaults=%d legacy_states=%d java=%d java_defaults=%d\n",
		len(palette), len(schematicIndices), len(bdxIndices), len(legacyToRuntime), len(legacyDefaults), len(legacyStateToRuntime), len(javaToRuntime), len(javaDefaults))
}
