//go:build linux || android || darwin || netbsd

package main

import "golang.org/x/sys/unix"

const (
	processMemoryLimitName        = "RLIMIT_AS"
	processMemoryLimitEnforcement = "rlimit_as"
)

func setProcessMemoryLimit(limit uint64) error {
	resource := &unix.Rlimit{Cur: limit, Max: limit}
	return unix.Setrlimit(unix.RLIMIT_AS, resource)
}
