package main

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"

	"github.com/Yeah114/blocks"
)

type entry struct {
	Block uint8 `json:"block"`
	Data  uint8 `json:"data"`
}

func main() {
	result := make(map[uint32]entry)
	for runtimeID := range blocks.MC_CURRENT.Blocks() {
		name, properties, found := blocks.RuntimeIDToState(uint32(runtimeID))
		if !found {
			continue
		}
		if !strings.Contains(name, ":") {
			name = "minecraft:" + name
		}
		reconstructed, _ := blocks.BlockNameAndStateToRuntimeID(name, properties)
		blockID, data, found := blocks.RuntimeIDToSchematic(reconstructed)
		if found {
			result[uint32(runtimeID)] = entry{Block: blockID, Data: data}
		}
	}
	encoded, err := json.Marshal(result)
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
	fmt.Printf("schematic reverse states=%d\n", len(result))
}
