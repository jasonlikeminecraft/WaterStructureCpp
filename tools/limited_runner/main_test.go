package main

import (
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestParseNumericMetricsStreamsKeyValueOutput(t *testing.T) {
	path := filepath.Join(t.TempDir(), "stdout.txt")
	if err := os.WriteFile(path, []byte("format=Schem\nparse_ms=12.5 get_chunks_ms=4\nchecksum=42 ignored=3\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	metrics := parseNumericMetrics(path)
	if metrics["parse_ms"] != 12.5 || metrics["get_chunks_ms"] != 4 || metrics["checksum"] != 42 {
		t.Fatalf("unexpected metrics: %#v", metrics)
	}
	if _, ok := metrics["ignored"]; ok {
		t.Fatalf("unexpected non-phase metric: %#v", metrics)
	}
}

func TestSummarizeMedianAndPeak(t *testing.T) {
	runs := []runResult{
		{DurationMs: 30, Termination: "exited", ExitCode: 0, Peak: memorySample{PeakRSS: 10}},
		{DurationMs: 10, Termination: "exited", ExitCode: 0, Peak: memorySample{PeakRSS: 20}},
		{DurationMs: 50, Termination: "timeout", ExitCode: 137, Peak: memorySample{PeakRSS: 30}},
	}
	summary := summarize(runs)
	if summary.Total != 3 || summary.Successful != 2 || summary.Failed != 1 {
		t.Fatalf("unexpected counts: %#v", summary)
	}
	if summary.MedianDurationMs != 20 || summary.MaxPeakRSSBytes != 30 {
		t.Fatalf("unexpected summary: %#v", summary)
	}
}

func TestEnvironmentOverridesReplaceExistingKey(t *testing.T) {
	os.Setenv("WATERSTRUCTURE_LIMITED_TEST", "old")
	t.Cleanup(func() { os.Unsetenv("WATERSTRUCTURE_LIMITED_TEST") })
	env := buildEnvironment([]string{"WATERSTRUCTURE_LIMITED_TEST=new", "WATERSTRUCTURE_LIMITED_NEW=value"})
	found := map[string]string{}
	for _, entry := range env {
		for i, char := range entry {
			if char == '=' {
				found[entry[:i]] = entry[i+1:]
				break
			}
		}
	}
	if found["WATERSTRUCTURE_LIMITED_TEST"] != "new" || found["WATERSTRUCTURE_LIMITED_NEW"] != "value" {
		t.Fatalf("unexpected environment: %#v", found)
	}
}

func TestMemoryFailureDetectionDoesNotLoadWholeLog(t *testing.T) {
	path := filepath.Join(t.TempDir(), "stderr.txt")
	if err := os.WriteFile(path, []byte("Traceback\nMemoryError\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if !looksLikeMemoryFailure(path) {
		t.Fatal("expected MemoryError to be classified as a memory-limit failure")
	}
}

func TestMergeCPUDistinguishesUnavailableFromMeasuredZero(t *testing.T) {
	value := cpuSample{}
	mergeCPU(&value, cpuSample{Unavailable: "unsupported"})
	if value.Available || value.Unavailable != "unsupported" {
		t.Fatalf("unexpected unavailable CPU sample: %#v", value)
	}
	mergeCPU(&value, cpuSample{Available: true, UserSeconds: 1.25, SystemSeconds: 0.5, TotalSeconds: 1.75, SplitAvailable: true})
	mergeCPU(&value, cpuSample{Available: true, UserSeconds: 1, SystemSeconds: 0.75, TotalSeconds: 1.75, SplitAvailable: true})
	if !value.Available || value.Unavailable != "" ||
		!value.SplitAvailable || math.Abs(value.TotalSeconds-1.75) > 1e-9 ||
		math.Abs(value.UserSeconds-1.25) > 1e-9 || math.Abs(value.SystemSeconds-0.75) > 1e-9 {
		t.Fatalf("unexpected merged CPU sample: %#v", value)
	}
}

func TestReportSerializesEnforcementTruthfully(t *testing.T) {
	value := runResult{
		HardTreeLimit: true,
		Enforcement:   "windows_job_object_process_and_job_memory",
	}
	encoded, err := json.Marshal(value)
	if err != nil {
		t.Fatal(err)
	}
	text := string(encoded)
	if !strings.Contains(text, `"hard_tree_limit":true`) ||
		!strings.Contains(text, `"enforcement":"windows_job_object_process_and_job_memory"`) {
		t.Fatalf("missing enforcement metadata: %s", text)
	}
}
