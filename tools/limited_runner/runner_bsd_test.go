//go:build darwin || freebsd || openbsd || netbsd || dragonfly

package main

import (
	"math"
	"testing"
)

func TestParsePSCPUTime(t *testing.T) {
	cases := map[string]float64{
		"00:01.25":   1.25,
		"01:02.50":   62.5,
		"02:03:04":   7384,
		"1-02:03:04": 93784,
	}
	for input, expected := range cases {
		actual, err := parsePSCPUTime(input)
		if err != nil || math.Abs(actual-expected) > 1e-9 {
			t.Fatalf("parsePSCPUTime(%q) = %v, %v; want %v", input, actual, err, expected)
		}
	}
}

func TestParseBSDPSLine(t *testing.T) {
	entry, err := parseBSDPSLine("42 7 42 1024 2048 01:02.50")
	if err != nil {
		t.Fatal(err)
	}
	if entry.pid != 42 || entry.ppid != 7 || entry.pgrp != 42 ||
		entry.rssBytes != 1024*1024 || entry.virtualBytes != 2048*1024 ||
		math.Abs(entry.cpuSeconds-62.5) > 1e-9 {
		t.Fatalf("unexpected ps entry: %#v", entry)
	}
}

func TestSelectBSDProcessTreeKeepsReparentedProcessGroup(t *testing.T) {
	const root = 42
	all := map[int]procEntry{
		// The root has already exited. PID 50 was reparented to init but remains
		// in the process group created for root. PID 51 moved to another group,
		// yet is still a descendant of the surviving group member.
		50: {pid: 50, ppid: 1, pgrp: root},
		51: {pid: 51, ppid: 50, pgrp: 51},
		60: {pid: 60, ppid: 1, pgrp: 60},
	}
	entries := selectBSDProcessTree(root, all)
	found := make(map[int]bool, len(entries))
	for _, entry := range entries {
		found[entry.pid] = true
	}
	if !found[50] || !found[51] {
		t.Fatalf("reparented process group was not retained: %#v", entries)
	}
	if found[60] {
		t.Fatalf("unrelated process was included: %#v", entries)
	}
}
