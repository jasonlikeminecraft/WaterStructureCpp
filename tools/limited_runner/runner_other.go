//go:build !windows && !linux && !android && !darwin && !freebsd && !openbsd && !netbsd && !dragonfly

package main

import "fmt"

// Unsupported Unix targets retain a compileable diagnostic rather than
// silently claiming that a hard memory limit is installed.
func platformName() string { return "unsupported-platform" }
func internalChild(_ []string) int {
	fmt.Println("limited_runner: platform does not support the process limiter")
	return 125
}
func startProcess(_ processSpec) (*processHandle, error) {
	return nil, fmt.Errorf("limited_runner: unsupported platform")
}
