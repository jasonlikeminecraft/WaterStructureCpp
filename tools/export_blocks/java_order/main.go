package main

import (
	"encoding/json"
	"fmt"
	"os"
	"reflect"
	"sort"
	"strings"
	"unsafe"

	"github.com/Yeah114/blocks"
	"github.com/Yeah114/blocks/describe"
)

func exposedOrder(value reflect.Value) reflect.Value {
	if value.CanInterface() {
		return value
	}
	return reflect.NewAt(value.Type(), unsafe.Pointer(value.UnsafeAddr())).Elem()
}

func canonicalOrderValue(value string) string {
	switch strings.ToLower(value) {
	case "true", "1b", "1":
		return "1"
	case "false", "0b", "0":
		return "0"
	default:
		return value
	}
}

func canonicalOrderKey(name string, states *describe.PropsForSearch) string {
	properties := make([]string, 0, len(*states))
	for _, property := range *states {
		properties = append(properties, property.Name+"="+canonicalOrderValue(property.Value.StringVal()))
	}
	sort.Strings(properties)
	if len(properties) == 0 {
		return "minecraft:" + name
	}
	return "minecraft:" + name + "[" + strings.Join(properties, ",") + "]"
}

type orderEntry struct {
	Runtime uint32 `json:"runtime"`
	Order   int    `json:"order"`
}

func main() {
	root := reflect.ValueOf(blocks.DefaultAnyToNemcConvertor).Elem()
	groups := exposedOrder(root.FieldByName("baseNames"))
	output := make(map[string]orderEntry)
	for _, name := range groups.MapKeys() {
		group := groups.MapIndex(name)
		ordered := exposedOrder(group.Elem().FieldByName("statesWithRtid"))
		for index := 0; index < ordered.Len(); index++ {
			item := ordered.Index(index)
			states := exposedOrder(item.FieldByName("states")).Interface().(*describe.PropsForSearch)
			if states == nil {
				continue
			}
			key := canonicalOrderKey(name.String(), states)
			if _, found := output[key]; !found {
				runtimeID := exposedOrder(item.FieldByName("rtid")).Uint()
				output[key] = orderEntry{Runtime: uint32(runtimeID), Order: index}
			}
		}
	}
	encoded, err := json.MarshalIndent(output, "", "  ")
	if err != nil {
		panic(err)
	}
	if len(os.Args) > 1 {
		if err := os.WriteFile(os.Args[1], encoded, 0o644); err != nil {
			panic(err)
		}
		return
	}
	fmt.Println(string(encoded))
}
