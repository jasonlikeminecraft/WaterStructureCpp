//go:build darwin || freebsd || openbsd || netbsd || dragonfly

package main

import (
	"bufio"
	"errors"
	"fmt"
	"math"
	"os/exec"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Darwin and the BSDs do not normally expose Linux-style /proc accounting.
// Use the native BSD ps interface as a low-memory stream and retain only the
// fields needed to construct the target's descendant tree.
type procEntry struct {
	pid          int
	ppid         int
	pgrp         int
	rssBytes     uint64
	virtualBytes uint64
	cpuSeconds   float64
}

const bsdSnapshotReuse = 5 * time.Millisecond

var bsdCache struct {
	sync.Mutex
	root       int
	capturedAt time.Time
	entries    []procEntry
	err        error
}

func validateProcessMonitor() error {
	_, err := exec.LookPath("ps")
	if err != nil {
		return fmt.Errorf("ps is required on %s: %w", runtime.GOOS, err)
	}
	return nil
}

func processTree(root int) []procEntry {
	entries, _ := cachedBSDProcessTree(root)
	return entries
}

func sampleProcessTreeMemory(root int) memorySample {
	entries, err := cachedBSDProcessTree(root)
	if err != nil {
		return memorySample{}
	}
	result := memorySample{}
	for _, entry := range entries {
		result.CurrentRSS = saturatingAddUint64(result.CurrentRSS, entry.rssBytes)
		result.CurrentVirtual = saturatingAddUint64(result.CurrentVirtual, entry.virtualBytes)
	}
	// ps exposes current RSS/VSZ, not an OS-maintained peak. The common sampler
	// merges repeated current values into the run peak.
	result.PeakRSS = result.CurrentRSS
	result.PeakVirtual = result.CurrentVirtual
	return result
}

func sampleProcessTreeCPU(root int) cpuSample {
	entries, err := cachedBSDProcessTree(root)
	if err != nil {
		return cpuSample{Unavailable: "ps process-tree accounting: " + err.Error()}
	}
	if len(entries) == 0 {
		return cpuSample{Unavailable: "process exited before ps CPU counters were readable"}
	}
	var total float64
	for _, entry := range entries {
		total += entry.cpuSeconds
	}
	// Portable BSD ps exposes cumulative total CPU time but not a consistent
	// user/system split across all supported hosts. Report the total honestly
	// and leave the split fields unavailable.
	return cpuSample{TotalSeconds: total, Available: true}
}

func cachedBSDProcessTree(root int) ([]procEntry, error) {
	bsdCache.Lock()
	defer bsdCache.Unlock()
	if bsdCache.root == root && !bsdCache.capturedAt.IsZero() &&
		time.Since(bsdCache.capturedAt) < bsdSnapshotReuse {
		return bsdCache.entries, bsdCache.err
	}
	entries, err := readBSDProcessTree(root)
	bsdCache.root = root
	bsdCache.capturedAt = time.Now()
	bsdCache.entries = entries
	bsdCache.err = err
	return entries, err
}

func readBSDProcessTree(root int) ([]procEntry, error) {
	command := exec.Command("ps", "-axo", "pid=,ppid=,pgid=,rss=,vsz=,time=")
	stdout, err := command.StdoutPipe()
	if err != nil {
		return nil, err
	}
	if err := command.Start(); err != nil {
		return nil, err
	}
	all := make(map[int]procEntry)
	scanner := bufio.NewScanner(stdout)
	scanner.Buffer(make([]byte, 4096), 64*1024)
	for scanner.Scan() {
		entry, parseErr := parseBSDPSLine(scanner.Text())
		if parseErr == nil && entry.pid > 0 {
			all[entry.pid] = entry
		}
	}
	scanErr := scanner.Err()
	waitErr := command.Wait()
	if scanErr != nil {
		return nil, scanErr
	}
	if waitErr != nil {
		return nil, waitErr
	}

	return selectBSDProcessTree(root, all), nil
}

func selectBSDProcessTree(root int, all map[int]procEntry) []procEntry {
	children := make(map[int][]int)
	for pid, entry := range all {
		children[entry.ppid] = append(children[entry.ppid], pid)
	}
	result := make([]procEntry, 0, 4)
	queue := []int{root}
	// startProcess creates a process group whose ID is the root PID. If the
	// root exits before a sample, BSD reparents its surviving children and a
	// PPID-only traversal loses them. Seed the traversal with every surviving
	// member of that process group, then follow their descendants as usual.
	for pid, entry := range all {
		if entry.pgrp == root && pid != root {
			queue = append(queue, pid)
		}
	}
	seen := make(map[int]bool)
	for len(queue) != 0 {
		pid := queue[0]
		queue = queue[1:]
		if seen[pid] {
			continue
		}
		seen[pid] = true
		if entry, ok := all[pid]; ok {
			result = append(result, entry)
		}
		queue = append(queue, children[pid]...)
	}
	return result
}

func parseBSDPSLine(line string) (procEntry, error) {
	fields := strings.Fields(line)
	if len(fields) != 6 {
		return procEntry{}, errors.New("unexpected ps field count")
	}
	pid, err := strconv.Atoi(fields[0])
	if err != nil {
		return procEntry{}, err
	}
	ppid, err := strconv.Atoi(fields[1])
	if err != nil {
		return procEntry{}, err
	}
	pgrp, err := strconv.Atoi(fields[2])
	if err != nil {
		return procEntry{}, err
	}
	rssKiB, err := strconv.ParseUint(fields[3], 10, 64)
	if err != nil {
		rssKiB = 0
	}
	virtualKiB, err := strconv.ParseUint(fields[4], 10, 64)
	if err != nil {
		virtualKiB = 0
	}
	cpuSeconds, err := parsePSCPUTime(fields[5])
	if err != nil {
		return procEntry{}, err
	}
	return procEntry{
		pid:          pid,
		ppid:         ppid,
		pgrp:         pgrp,
		rssBytes:     kibibytesToBytes(rssKiB),
		virtualBytes: kibibytesToBytes(virtualKiB),
		cpuSeconds:   cpuSeconds,
	}, nil
}

func parsePSCPUTime(value string) (float64, error) {
	value = strings.TrimSpace(value)
	if value == "" {
		return 0, errors.New("empty ps CPU time")
	}
	var days float64
	if dash := strings.IndexByte(value, '-'); dash >= 0 {
		parsed, err := strconv.ParseFloat(value[:dash], 64)
		if err != nil {
			return 0, err
		}
		days = parsed
		value = value[dash+1:]
	}
	parts := strings.Split(value, ":")
	if len(parts) < 2 || len(parts) > 3 {
		return 0, errors.New("invalid ps CPU time")
	}
	seconds, err := strconv.ParseFloat(parts[len(parts)-1], 64)
	if err != nil {
		return 0, err
	}
	minutes, err := strconv.ParseFloat(parts[len(parts)-2], 64)
	if err != nil {
		return 0, err
	}
	hours := 0.0
	if len(parts) == 3 {
		hours, err = strconv.ParseFloat(parts[0], 64)
		if err != nil {
			return 0, err
		}
	}
	return days*24*60*60 + hours*60*60 + minutes*60 + seconds, nil
}

func kibibytesToBytes(value uint64) uint64 {
	if value > math.MaxUint64/1024 {
		return math.MaxUint64
	}
	return value * 1024
}

func saturatingAddUint64(left, right uint64) uint64 {
	if right > math.MaxUint64-left {
		return math.MaxUint64
	}
	return left + right
}
