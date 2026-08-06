package main

import (
	"testing"

	"github.com/Yeah114/blocks"
)

func TestLegacyAliasUsedByMianYangFixture(t *testing.T) {
	runtimeID, found := blocks.LegacyBlockToRuntimeID("minecraft:stone_block_slab", 0)
	if !found {
		t.Fatal("stone_block_slab legacy alias is missing")
	}
	name, states, found := blocks.RuntimeIDToState(runtimeID)
	if !found {
		t.Fatalf("legacy alias runtime ID %d cannot be reversed", runtimeID)
	}
	t.Logf("runtime_id=%d name=%s states=%v", runtimeID, name, states)
}

func TestLegacyAliasesCanonicalizeCaseWithoutDroppingSources(t *testing.T) {
	aliases := make(map[string]map[string]struct{})
	addLegacyAlias(aliases, "tripWire")
	addLegacyAlias(aliases, "minecraft:tripwire")
	values := aliases["minecraft:tripwire"]
	if len(aliases) != 1 || len(values) != 2 {
		t.Fatalf("legacy aliases = %#v, want one canonical key with two source spellings", aliases)
	}
}
