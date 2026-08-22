//go:build windows

package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestWindowsJobReportsHardTreeEnforcement(t *testing.T) {
	if os.Getenv("LIMITED_RUNNER_WINDOWS_HELPER") == "1" {
		return
	}
	temporary := t.TempDir()
	result := executeOneResult(processSpec{
		command:     os.Args[0],
		args:        []string{"-test.run=^TestWindowsJobReportsHardTreeEnforcement$"},
		env:         []string{"LIMITED_RUNNER_WINDOWS_HELPER=1"},
		stdoutPath:  filepath.Join(temporary, "stdout.txt"),
		stderrPath:  filepath.Join(temporary, "stderr.txt"),
		limitBytes:  500 * 1024 * 1024,
		sampleEvery: 10 * time.Millisecond,
	}, 30*time.Second)
	if result.Termination != "exited" || result.ExitCode != 0 {
		t.Fatalf("limited helper failed: %#v", result)
	}
	if !result.HardTreeLimit || !strings.Contains(result.Enforcement, "windows_job_object") {
		t.Fatalf("job enforcement was not reported: %#v", result)
	}
	if !result.CPUAvailable || !result.CPUSplitAvailable {
		t.Fatalf("job CPU accounting was not reported: %#v", result)
	}
}

func TestWindowsEnvironmentOverridesAreCaseInsensitive(t *testing.T) {
	values := buildEnvironment([]string{"path=limited-runner-test-path"})
	found := 0
	for _, value := range values {
		key, raw, ok := strings.Cut(value, "=")
		if ok && strings.EqualFold(key, "PATH") {
			found++
			if raw != "limited-runner-test-path" {
				t.Fatalf("PATH override was not applied: %q", value)
			}
		}
	}
	if found != 1 {
		t.Fatalf("expected exactly one case-insensitive PATH entry, got %d", found)
	}
}
