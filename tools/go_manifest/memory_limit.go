package main

import (
	"fmt"
	"os"
	"runtime/debug"
	"strconv"
)

const defaultManifestMemoryMiB = 1024

// configureManifestMemory installs both the Go heap limit and, on Windows, a
// process commit limit. The latter is the important guard: GOMEMLIMIT is a GC
// target, not a hard cap, and cannot protect the host from a single oversized
// allocation in a third-party reader.
func configureManifestMemory() error {
	limitMiB := defaultManifestMemoryMiB
	if raw := os.Getenv("WATERSTRUCTURE_MANIFEST_MEMORY_MIB"); raw != "" {
		parsed, err := strconv.Atoi(raw)
		if err != nil || parsed < 256 || parsed > 16384 {
			return fmt.Errorf("invalid WATERSTRUCTURE_MANIFEST_MEMORY_MIB %q (expected 256..16384)", raw)
		}
		limitMiB = parsed
	}
	// Keep native allocations and the runtime itself below the process limit.
	heapLimit := int64(limitMiB-128) * 1024 * 1024
	if heapLimit < 128*1024*1024 {
		heapLimit = 128 * 1024 * 1024
	}
	debug.SetMemoryLimit(heapLimit)
	return installProcessMemoryLimit(uint64(limitMiB) * 1024 * 1024)
}
