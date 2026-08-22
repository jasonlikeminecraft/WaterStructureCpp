//go:build freebsd || dragonfly

package main

import (
	"fmt"
	"math"

	"golang.org/x/sys/unix"
)

const (
	processMemoryLimitName        = "RLIMIT_AS"
	processMemoryLimitEnforcement = "rlimit_as"
)

func setProcessMemoryLimit(limit uint64) error {
	if limit > math.MaxInt64 {
		return fmt.Errorf("RLIMIT_AS value %d exceeds int64", limit)
	}
	value := int64(limit)
	resource := &unix.Rlimit{Cur: value, Max: value}
	return unix.Setrlimit(unix.RLIMIT_AS, resource)
}
