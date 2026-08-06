package main

import (
	"encoding/json"
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"

	"github.com/Yeah114/blocks"
)

type property struct {
	Type  string `json:"type"`
	Value string `json:"value"`
}

type javaState struct {
	Runtime uint32              `json:"runtime"`
	Name    string              `json:"name"`
	States  map[string]property `json:"states,omitempty"`
}

func typedProperty(value any) (property, error) {
	switch value := value.(type) {
	case bool:
		if value {
			return property{Type: "byte", Value: "1"}, nil
		}
		return property{Type: "byte", Value: "0"}, nil
	case uint8:
		return property{Type: "byte", Value: strconv.FormatUint(uint64(value), 10)}, nil
	case int8:
		return property{Type: "byte", Value: strconv.FormatInt(int64(value), 10)}, nil
	case int32:
		return property{Type: "int", Value: strconv.FormatInt(int64(value), 10)}, nil
	case int:
		return property{Type: "int", Value: strconv.Itoa(value)}, nil
	case string:
		return property{Type: "string", Value: value}, nil
	default:
		return property{}, fmt.Errorf("unsupported Java state type %T", value)
	}
}

func main() {
	output := make([]javaState, 0, len(blocks.MC_CURRENT.Blocks()))
	for runtimeID := range blocks.MC_CURRENT.Blocks() {
		name, properties, found := blocks.RuntimeIDToJavaBlockNameAndState(uint32(runtimeID))
		if !found || name == "" {
			if uint32(runtimeID) == blocks.AIR_RUNTIMEID {
				output = append(output, javaState{Runtime: uint32(runtimeID), Name: "minecraft:air"})
			}
			continue
		}
		if !strings.Contains(name, ":") {
			name = "minecraft:" + name
		}
		entry := javaState{Runtime: uint32(runtimeID), Name: name}
		if len(properties) != 0 {
			entry.States = make(map[string]property, len(properties))
			keys := make([]string, 0, len(properties))
			for key := range properties {
				keys = append(keys, key)
			}
			sort.Strings(keys)
			for _, key := range keys {
				value, err := typedProperty(properties[key])
				if err != nil {
					panic(fmt.Errorf("runtime ID %d property %s: %w", runtimeID, key, err))
				}
				entry.States[key] = value
			}
		}
		output = append(output, entry)
	}
	encoded, err := json.Marshal(output)
	if err != nil {
		panic(err)
	}
	if len(os.Args) < 2 {
		fmt.Println(string(encoded))
		return
	}
	if err := os.WriteFile(os.Args[1], encoded, 0o644); err != nil {
		panic(err)
	}
	fmt.Printf("java runtime states=%d\n", len(output))
}
