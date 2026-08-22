//go:build linux || android

package main

import (
	"errors"
	"fmt"
	"os"
	"strconv"
	"strings"
)

// procEntry is the small subset of /proc/<pid>/stat needed for process-tree
// accounting. Reading stat directly keeps the monitor independent from ps and
// works in the read-only procfs normally exposed by containers and Termux.
type procEntry struct {
	pid       int
	ppid      int
	pgrp      int
	userTicks uint64
	sysTicks  uint64
}

func validateProcessMonitor() error { return nil }

func readProcEntry(pid int) (procEntry, error) {
	data, err := os.ReadFile(fmt.Sprintf("/proc/%d/stat", pid))
	if err != nil {
		return procEntry{}, err
	}
	text := string(data)
	close := strings.LastIndexByte(text, ')')
	if close < 0 || close+2 >= len(text) {
		return procEntry{}, errors.New("malformed /proc stat")
	}
	fields := strings.Fields(text[close+2:])
	// fields[0] is state (field 3); ppid/pgrp are fields 4/5 and utime/
	// stime are fields 14/15.
	if len(fields) <= 12 {
		return procEntry{}, errors.New("truncated /proc stat")
	}
	parseInt := func(index int) (int, error) { return strconv.Atoi(fields[index]) }
	ppid, err := parseInt(1)
	if err != nil {
		return procEntry{}, err
	}
	pgrp, err := parseInt(2)
	if err != nil {
		return procEntry{}, err
	}
	user, err := strconv.ParseUint(fields[11], 10, 64)
	if err != nil {
		return procEntry{}, err
	}
	system, err := strconv.ParseUint(fields[12], 10, 64)
	if err != nil {
		return procEntry{}, err
	}
	return procEntry{pid: pid, ppid: ppid, pgrp: pgrp, userTicks: user, sysTicks: system}, nil
}

func processTree(root int) []procEntry {
	entries, err := os.ReadDir("/proc")
	if err != nil {
		return nil
	}
	all := make(map[int]procEntry, len(entries))
	for _, entry := range entries {
		pid, parseErr := strconv.Atoi(entry.Name())
		if parseErr != nil {
			continue
		}
		if value, readErr := readProcEntry(pid); readErr == nil {
			all[pid] = value
		}
	}
	children := make(map[int][]int)
	for pid, entry := range all {
		children[entry.ppid] = append(children[entry.ppid], pid)
	}
	result := make([]procEntry, 0, 4)
	queue := []int{root}
	seen := map[int]bool{}
	for len(queue) > 0 {
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

func sampleProcessTreeCPU(pid int) cpuSample {
	entries := processTree(pid)
	if len(entries) == 0 {
		return cpuSample{Unavailable: "process exited before /proc CPU counters were readable"}
	}
	var userTicks, systemTicks uint64
	for _, entry := range entries {
		userTicks += entry.userTicks
		systemTicks += entry.sysTicks
	}
	// Linux and Android expose USER_HZ=100 on the architectures supported by
	// this project. Keep the constant local and report unavailable above when
	// procfs cannot provide a trustworthy sample.
	const ticksPerSecond = 100.0
	return cpuSample{
		UserSeconds:    float64(userTicks) / ticksPerSecond,
		SystemSeconds:  float64(systemTicks) / ticksPerSecond,
		TotalSeconds:   float64(userTicks+systemTicks) / ticksPerSecond,
		SplitAvailable: true,
		Available:      true,
	}
}

func sampleProcessTreeMemory(pid int) memorySample {
	result := memorySample{}
	for _, entry := range processTree(pid) {
		addMemory(&result, sampleLinuxProcessOne(entry.pid))
	}
	return normalizeMemory(result)
}

func sampleLinuxProcessOne(pid int) memorySample {
	result := memorySample{}
	if raw, err := os.ReadFile(fmt.Sprintf("/proc/%d/status", pid)); err == nil {
		for _, line := range strings.Split(string(raw), "\n") {
			parts := strings.Fields(line)
			if len(parts) < 2 {
				continue
			}
			value, err := strconv.ParseUint(parts[1], 10, 64)
			if err != nil {
				continue
			}
			value *= 1024 // status values are kB
			switch parts[0] {
			case "VmRSS:":
				result.CurrentRSS = value
			case "VmHWM:":
				result.PeakRSS = value
			case "VmSize:":
				result.CurrentVirtual = value
			case "VmPeak:":
				result.PeakVirtual = value
			}
		}
	}
	if raw, err := os.ReadFile(fmt.Sprintf("/proc/%d/smaps_rollup", pid)); err == nil {
		for _, line := range strings.Split(string(raw), "\n") {
			parts := strings.Fields(line)
			if len(parts) < 2 {
				continue
			}
			value, err := strconv.ParseUint(parts[1], 10, 64)
			if err != nil {
				continue
			}
			value *= 1024
			if parts[0] == "Private_Clean:" || parts[0] == "Private_Dirty:" {
				result.CurrentPrivate += value
			}
		}
		result.PeakPrivate = result.CurrentPrivate
	}
	return result
}
