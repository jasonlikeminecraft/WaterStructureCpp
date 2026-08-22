//go:build openbsd

package main

import "golang.org/x/sys/unix"

// OpenBSD does not expose RLIMIT_AS. RLIMIT_DATA is its closest enforceable
// per-process memory limit; the parent still samples aggregate process-tree
// RSS/VSZ and terminates the tracked tree when it reaches the configured cap.
const (
	processMemoryLimitName        = "RLIMIT_DATA"
	processMemoryLimitEnforcement = "rlimit_data"
)

func setProcessMemoryLimit(limit uint64) error {
	resource := &unix.Rlimit{Cur: limit, Max: limit}
	return unix.Setrlimit(unix.RLIMIT_DATA, resource)
}
