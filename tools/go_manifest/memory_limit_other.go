//go:build !windows

package main

func installProcessMemoryLimit(_ uint64) error { return nil }
