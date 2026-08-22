// Command limited_runner executes one or more benchmark/conversion commands
// in isolated child processes.  It is deliberately independent from the
// WaterStructure library so it can also be used for Go/oracle comparisons.
//
// The runner keeps stdout/stderr in files and samples memory without reading
// the command's output into memory.  A platform-specific implementation
// provides the hard process limit (Windows Job Object, Unix RLIMIT/cgroup).
package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	reportSchema       = 1
	defaultLimitMiB    = 500
	defaultSampleMilli = 100
)

// processExit is sent exactly once by a platform process handle.
type processExit struct {
	exitCode int
	signal   string
	err      error
}

// memorySample uses bytes throughout.  A platform may not expose every
// counter; zero means that counter is unavailable.
type memorySample struct {
	CurrentRSS     uint64 `json:"current_rss_bytes,omitempty"`
	PeakRSS        uint64 `json:"peak_rss_bytes,omitempty"`
	CurrentPrivate uint64 `json:"current_private_bytes,omitempty"`
	PeakPrivate    uint64 `json:"peak_private_bytes,omitempty"`
	CurrentVirtual uint64 `json:"current_virtual_bytes,omitempty"`
	PeakVirtual    uint64 `json:"peak_virtual_bytes,omitempty"`
}

// cpuSample contains cumulative CPU time observed for the child process and
// any descendants that the platform can account for.  A platform must leave
// Available false when it cannot provide trustworthy counters; zero values are
// therefore never presented as a measured "0 CPU" result.
type cpuSample struct {
	UserSeconds    float64 `json:"user_seconds,omitempty"`
	SystemSeconds  float64 `json:"system_seconds,omitempty"`
	TotalSeconds   float64 `json:"total_seconds,omitempty"`
	SplitAvailable bool    `json:"split_available"`
	Available      bool    `json:"available"`
	Unavailable    string  `json:"unavailable_reason,omitempty"`
}

type processHandle struct {
	pid           int
	backend       string
	hardTreeLimit bool
	enforcement   string
	done          <-chan processExit
	kill          func() error
	sample        func() memorySample
	sampleCPU     func() cpuSample
	limitReached  func() bool
	close         func()
}

type processSpec struct {
	command       string
	args          []string
	workingDir    string
	env           []string
	stdoutPath    string
	stderrPath    string
	limitBytes    uint64
	sampleEvery   time.Duration
	processNumber int
}

type runResult struct {
	Index             int                `json:"index"`
	StartedAt         string             `json:"started_at"`
	FinishedAt        string             `json:"finished_at"`
	DurationMs        float64            `json:"duration_ms"`
	Command           []string           `json:"command"`
	WorkingDirectory  string             `json:"working_directory,omitempty"`
	Backend           string             `json:"backend,omitempty"`
	HardTreeLimit     bool               `json:"hard_tree_limit"`
	Enforcement       string             `json:"enforcement,omitempty"`
	PID               int                `json:"pid,omitempty"`
	ExitCode          int                `json:"exit_code"`
	Signal            string             `json:"signal,omitempty"`
	Termination       string             `json:"termination"`
	MemoryExceeded    bool               `json:"memory_limit_exceeded"`
	TimedOut          bool               `json:"timed_out"`
	Peak              memorySample       `json:"memory"`
	CPUUserSeconds    float64            `json:"cpu_user_seconds,omitempty"`
	CPUSystemSeconds  float64            `json:"cpu_system_seconds,omitempty"`
	CPUTotalSeconds   float64            `json:"cpu_total_seconds,omitempty"`
	CPUSplitAvailable bool               `json:"cpu_split_available"`
	CPUUtilization    float64            `json:"cpu_utilization_percent,omitempty"`
	CPUAvailable      bool               `json:"cpu_available"`
	CPUUnavailable    string             `json:"cpu_unavailable_reason,omitempty"`
	LimitBytes        uint64             `json:"limit_bytes"`
	StdoutPath        string             `json:"stdout_path,omitempty"`
	StderrPath        string             `json:"stderr_path,omitempty"`
	StdoutBytes       int64              `json:"stdout_bytes,omitempty"`
	StderrBytes       int64              `json:"stderr_bytes,omitempty"`
	NumericMetrics    map[string]float64 `json:"metrics,omitempty"`
	StartError        string             `json:"start_error,omitempty"`
	Error             string             `json:"error,omitempty"`
}

type runReport struct {
	Schema     int         `json:"schema"`
	Tool       string      `json:"tool"`
	Platform   string      `json:"platform"`
	LimitMiB   int         `json:"limit_mib"`
	SampleMs   int         `json:"sample_interval_ms"`
	Repeat     int         `json:"repeat"`
	Warmup     int         `json:"warmup"`
	Command    []string    `json:"command"`
	StartedAt  string      `json:"started_at"`
	FinishedAt string      `json:"finished_at"`
	OK         bool        `json:"ok"`
	Runs       []runResult `json:"runs"`
	Summary    runSummary  `json:"summary"`
}

type runSummary struct {
	Total               int     `json:"total"`
	Successful          int     `json:"successful"`
	Failed              int     `json:"failed"`
	MedianDurationMs    float64 `json:"median_duration_ms"`
	MaxDurationMs       float64 `json:"max_duration_ms"`
	MaxPeakRSSBytes     uint64  `json:"max_peak_rss_bytes"`
	MaxPeakPrivateBytes uint64  `json:"max_peak_private_bytes"`
	MaxPeakVirtualBytes uint64  `json:"max_peak_virtual_bytes"`
}

type stringList []string

func (v *stringList) String() string { return strings.Join(*v, ",") }

func (v *stringList) Set(value string) error {
	if strings.IndexByte(value, '=') <= 0 {
		return fmt.Errorf("environment must be KEY=VALUE, got %q", value)
	}
	*v = append(*v, value)
	return nil
}

type options struct {
	limitMiB       int
	sampleMs       int
	timeout        time.Duration
	repeat         int
	warmup         int
	output         string
	stdoutDir      string
	keepOutput     bool
	continueErrors bool
	workingDir     string
	env            stringList
}

func main() {
	if len(os.Args) > 1 && os.Args[1] == "--internal-child" {
		os.Exit(internalChild(os.Args[2:]))
	}

	var cfg options
	flags := flag.NewFlagSet("limited_runner", flag.ContinueOnError)
	flags.SetOutput(os.Stderr)
	flags.IntVar(&cfg.limitMiB, "limit-mib", defaultLimitMiB,
		"memory limit per run in MiB (tree-hard where supported; maximum 500)")
	flags.IntVar(&cfg.sampleMs, "sample-ms", defaultSampleMilli,
		"memory sampling interval in milliseconds")
	flags.DurationVar(&cfg.timeout, "timeout", 0,
		"terminate a run after this duration (0 means no timeout)")
	flags.IntVar(&cfg.repeat, "repeat", 1, "number of measured runs")
	flags.IntVar(&cfg.warmup, "warmup", 0, "number of unreported warm-up runs")
	flags.StringVar(&cfg.output, "output", "", "JSON report path (empty writes to stdout)")
	flags.StringVar(&cfg.stdoutDir, "stdout-dir", "", "directory for streamed child stdout/stderr files")
	flags.BoolVar(&cfg.keepOutput, "keep-output", false, "keep automatically-created output files")
	flags.BoolVar(&cfg.continueErrors, "continue-on-error", false,
		"run all repetitions even if one child fails")
	flags.StringVar(&cfg.workingDir, "cwd", "", "child working directory")
	flags.Var(&cfg.env, "env", "add/replace a child environment variable (repeatable)")
	flags.Usage = func() {
		fmt.Fprintln(os.Stderr, "usage: limited_runner [options] -- command [args ...]")
		flags.PrintDefaults()
	}
	if err := flags.Parse(os.Args[1:]); err != nil {
		os.Exit(64)
	}
	command := flags.Args()
	if len(command) > 0 && command[0] == "--" {
		command = command[1:]
	}
	if err := validateOptions(cfg, command); err != nil {
		fmt.Fprintln(os.Stderr, "limited_runner:", err)
		os.Exit(64)
	}

	report, exitCode := execute(cfg, command)
	if err := writeReport(cfg.output, report); err != nil {
		fmt.Fprintln(os.Stderr, "limited_runner:", err)
		if exitCode == 0 {
			exitCode = 70
		}
	}
	if cfg.output == "" {
		// writeReport already emitted JSON to stdout when no path was given.
	}
	os.Exit(exitCode)
}

func validateOptions(cfg options, command []string) error {
	if len(command) == 0 || strings.TrimSpace(command[0]) == "" {
		return errors.New("a command is required after --")
	}
	if cfg.limitMiB < 64 || cfg.limitMiB > defaultLimitMiB {
		return fmt.Errorf("limit-mib must be between 64 and %d", defaultLimitMiB)
	}
	if cfg.sampleMs < 10 || cfg.sampleMs > 60_000 {
		return errors.New("sample-ms must be between 10 and 60000")
	}
	if cfg.repeat < 1 || cfg.repeat > 10_000 {
		return errors.New("repeat must be between 1 and 10000")
	}
	if cfg.warmup < 0 || cfg.warmup > 1000 {
		return errors.New("warmup must be between 0 and 1000")
	}
	if cfg.stdoutDir != "" {
		if info, err := os.Stat(cfg.stdoutDir); err == nil && !info.IsDir() {
			return fmt.Errorf("stdout-dir is not a directory: %s", cfg.stdoutDir)
		}
	}
	return nil
}

func execute(cfg options, command []string) (runReport, int) {
	started := time.Now()
	report := runReport{
		Schema: reportSchema, Tool: "waterstructure-limited-runner",
		Platform: platformName(), LimitMiB: cfg.limitMiB, SampleMs: cfg.sampleMs,
		Repeat: cfg.repeat, Warmup: cfg.warmup, Command: append([]string(nil), command...),
		StartedAt: started.UTC().Format(time.RFC3339Nano), Runs: make([]runResult, 0, cfg.repeat),
	}
	limitBytes := uint64(cfg.limitMiB) * 1024 * 1024
	base, temporary := outputBase(cfg.output)
	if cfg.stdoutDir != "" {
		base = filepath.Join(cfg.stdoutDir, filepath.Base(base))
	}
	if err := os.MkdirAll(filepath.Dir(base), 0o755); err != nil {
		return report, 70
	}
	for i := 0; i < cfg.warmup; i++ {
		stdoutPath, stderrPath := streamPaths(base, i+1, cfg.warmup+cfg.repeat, true)
		warmupResult, warmupErr := executeOne(processSpec{
			command: command[0], args: command[1:], workingDir: cfg.workingDir,
			env: append([]string(nil), cfg.env...), stdoutPath: stdoutPath, stderrPath: stderrPath,
			limitBytes: limitBytes, sampleEvery: time.Duration(cfg.sampleMs) * time.Millisecond,
			processNumber: i + 1,
		}, cfg.timeout)
		if warmupErr != nil {
			if temporary && !cfg.keepOutput {
				_ = os.Remove(warmupResult.StdoutPath)
				_ = os.Remove(warmupResult.StderrPath)
			}
			if !cfg.continueErrors {
				break
			}
		} else if temporary && !cfg.keepOutput {
			_ = os.Remove(warmupResult.StdoutPath)
			_ = os.Remove(warmupResult.StderrPath)
		}
	}
	exitCode := 0
	for i := 0; i < cfg.repeat; i++ {
		runIndex := cfg.warmup + i + 1
		stdoutPath, stderrPath := streamPaths(base, runIndex, cfg.warmup+cfg.repeat, false)
		result := executeOneResult(processSpec{
			command: command[0], args: command[1:], workingDir: cfg.workingDir,
			env: append([]string(nil), cfg.env...), stdoutPath: stdoutPath, stderrPath: stderrPath,
			limitBytes: limitBytes, sampleEvery: time.Duration(cfg.sampleMs) * time.Millisecond,
			processNumber: runIndex,
		}, cfg.timeout)
		report.Runs = append(report.Runs, result)
		if result.Termination != "exited" || result.ExitCode != 0 {
			exitCode = 1
			if result.MemoryExceeded {
				exitCode = 125
			}
			if !cfg.continueErrors {
				break
			}
		}
	}
	report.FinishedAt = time.Now().UTC().Format(time.RFC3339Nano)
	report.Summary = summarize(report.Runs)
	report.OK = report.Summary.Failed == 0 && len(report.Runs) == cfg.repeat
	if temporary && !cfg.keepOutput {
		for index := range report.Runs {
			_ = os.Remove(report.Runs[index].StdoutPath)
			_ = os.Remove(report.Runs[index].StderrPath)
			report.Runs[index].StdoutPath = ""
			report.Runs[index].StderrPath = ""
		}
	}
	return report, exitCode
}

func outputBase(output string) (string, bool) {
	if output != "" {
		absolute, err := filepath.Abs(output)
		if err == nil {
			output = absolute
		}
		return strings.TrimSuffix(output, filepath.Ext(output)), false
	}
	base := filepath.Join(os.TempDir(), "waterstructure-limited-"+strconv.FormatInt(time.Now().UnixNano(), 10))
	return base, true
}

func streamPaths(base string, index, total int, warmup bool) (string, string) {
	suffix := ""
	if total == 1 {
		suffix = ".warmup"
		if !warmup {
			suffix = ""
		}
	} else {
		suffix = fmt.Sprintf(".run-%03d", index)
		if warmup {
			suffix += ".warmup"
		}
	}
	return base + suffix + ".stdout.txt", base + suffix + ".stderr.txt"
}

func executeOneResult(spec processSpec, timeout time.Duration) runResult {
	started := time.Now()
	result := runResult{
		Index: spec.processNumber, StartedAt: started.UTC().Format(time.RFC3339Nano),
		Command: append([]string{spec.command}, spec.args...), WorkingDirectory: spec.workingDir,
		LimitBytes: spec.limitBytes, StdoutPath: spec.stdoutPath, StderrPath: spec.stderrPath,
		Termination: "start_error", ExitCode: -1,
	}
	handle, err := startProcess(spec)
	if err != nil {
		result.StartError = err.Error()
		result.FinishedAt = time.Now().UTC().Format(time.RFC3339Nano)
		result.DurationMs = float64(time.Since(started).Microseconds()) / 1000
		return result
	}
	result.PID, result.Backend = handle.pid, handle.backend
	result.HardTreeLimit, result.Enforcement = handle.hardTreeLimit, handle.enforcement
	peak := memorySample{}
	cpu := cpuSample{}
	limitHit := false
	timedOut := false
	var exit processExit
	ticker := time.NewTicker(spec.sampleEvery)
	defer ticker.Stop()
	var timeoutTimer *time.Timer
	var timeoutC <-chan time.Time
	if timeout > 0 {
		timeoutTimer = time.NewTimer(timeout)
		timeoutC = timeoutTimer.C
		defer timeoutTimer.Stop()
	}
	for {
		select {
		case exit = <-handle.done:
			goto finished
		case <-ticker.C:
			mergeMemory(&peak, handle.sample())
			if handle.sampleCPU != nil {
				mergeCPU(&cpu, handle.sampleCPU())
			}
			if ((handle.limitReached != nil && handle.limitReached()) || exceedsMemory(peak, spec.limitBytes)) && !limitHit {
				limitHit = true
				_ = handle.kill()
			}
		case <-timeoutC:
			timedOut = true
			_ = handle.kill()
			timeoutC = nil
		}
	}

finished:
	mergeMemory(&peak, handle.sample())
	if handle.sampleCPU != nil {
		mergeCPU(&cpu, handle.sampleCPU())
	}
	platformLimitHit := handle.limitReached != nil && handle.limitReached()
	if timedOut {
		// Wait for the group after a timeout kill; the platform handle's done
		// channel only closes once all process handles are released.
		result.TimedOut = true
	}
	result.MemoryExceeded = limitHit
	if platformLimitHit {
		result.MemoryExceeded = true
	}
	result.Peak = peak
	result.CPUUserSeconds = cpu.UserSeconds
	result.CPUSystemSeconds = cpu.SystemSeconds
	result.CPUTotalSeconds = cpu.TotalSeconds
	result.CPUSplitAvailable = cpu.SplitAvailable
	result.CPUAvailable = cpu.Available
	result.CPUUnavailable = cpu.Unavailable
	result.ExitCode, result.Signal = exit.exitCode, exit.signal
	result.Error = errorString(exit.err)
	handle.close()
	if !result.MemoryExceeded && !timedOut &&
		(isMemoryExitStatus(exit.exitCode, exit.signal) || looksLikeMemoryFailure(spec.stderrPath)) {
		result.MemoryExceeded = true
	}
	switch {
	case result.MemoryExceeded:
		result.Termination = "LIMIT_EXCEEDED"
	case timedOut:
		result.Termination = "timeout"
	case exit.signal != "":
		result.Termination = "signal"
	case exit.err != nil:
		result.Termination = "wait_error"
	default:
		result.Termination = "exited"
	}
	result.FinishedAt = time.Now().UTC().Format(time.RFC3339Nano)
	result.DurationMs = float64(time.Since(started).Microseconds()) / 1000
	if cpu.Available && result.DurationMs > 0 {
		totalCPU := cpu.TotalSeconds
		if totalCPU == 0 {
			totalCPU = cpu.UserSeconds + cpu.SystemSeconds
		}
		result.CPUUtilization = totalCPU /
			(result.DurationMs / 1000) * 100
	}
	result.StdoutBytes = fileSize(spec.stdoutPath)
	result.StderrBytes = fileSize(spec.stderrPath)
	result.NumericMetrics = parseNumericMetrics(spec.stdoutPath)
	return result
}

func isMemoryExitStatus(_ int, signal string) bool {
	status := strings.ToLower(signal)
	return strings.Contains(status, "0xc0000017") || // STATUS_NO_MEMORY
		strings.Contains(status, "0xc000012d") || // STATUS_COMMITMENT_LIMIT
		strings.Contains(status, "0xc00000a1") // STATUS_WORKING_SET_QUOTA
}

func looksLikeMemoryFailure(path string) bool {
	file, err := os.Open(path)
	if err != nil {
		return false
	}
	defer file.Close()
	scanner := bufio.NewScanner(file)
	scanner.Buffer(make([]byte, 4096), 1024*1024)
	patterns := []string{"memoryerror", "std::bad_alloc", "out of memory", "cannot allocate memory", "not enough memory", "allocation failed"}
	for scanner.Scan() {
		line := strings.ToLower(scanner.Text())
		for _, pattern := range patterns {
			if strings.Contains(line, pattern) {
				return true
			}
		}
	}
	return false
}

// executeOne exists for warm-up calls where only an error is relevant.
func executeOne(spec processSpec, timeout time.Duration) (runResult, error) {
	result := executeOneResult(spec, timeout)
	if result.Termination != "exited" || result.ExitCode != 0 {
		return result, fmt.Errorf("warm-up run %d failed: %s", result.Index, result.Termination)
	}
	return result, nil
}

func errorString(err error) string {
	if err == nil {
		return ""
	}
	return err.Error()
}

func mergeMemory(dst *memorySample, value memorySample) {
	if value.CurrentRSS > dst.CurrentRSS {
		dst.CurrentRSS = value.CurrentRSS
	}
	if value.PeakRSS > dst.PeakRSS {
		dst.PeakRSS = value.PeakRSS
	}
	if value.CurrentPrivate > dst.CurrentPrivate {
		dst.CurrentPrivate = value.CurrentPrivate
	}
	if value.PeakPrivate > dst.PeakPrivate {
		dst.PeakPrivate = value.PeakPrivate
	}
	if value.CurrentVirtual > dst.CurrentVirtual {
		dst.CurrentVirtual = value.CurrentVirtual
	}
	if value.PeakVirtual > dst.PeakVirtual {
		dst.PeakVirtual = value.PeakVirtual
	}
}

func mergeCPU(dst *cpuSample, value cpuSample) {
	if !value.Available {
		if !dst.Available && dst.Unavailable == "" {
			dst.Unavailable = value.Unavailable
		}
		return
	}
	if value.UserSeconds > dst.UserSeconds {
		dst.UserSeconds = value.UserSeconds
	}
	if value.SystemSeconds > dst.SystemSeconds {
		dst.SystemSeconds = value.SystemSeconds
	}
	if value.TotalSeconds > dst.TotalSeconds {
		dst.TotalSeconds = value.TotalSeconds
	}
	dst.SplitAvailable = dst.SplitAvailable || value.SplitAvailable
	dst.Available = true
	dst.Unavailable = ""
}

func exceedsMemory(sample memorySample, limit uint64) bool {
	return (sample.CurrentRSS != 0 && sample.CurrentRSS > limit) ||
		(sample.CurrentPrivate != 0 && sample.CurrentPrivate > limit) ||
		(sample.CurrentVirtual != 0 && sample.CurrentVirtual > limit)
}

func fileSize(path string) int64 {
	if path == "" {
		return 0
	}
	info, err := os.Stat(path)
	if err != nil {
		return 0
	}
	return info.Size()
}

func parseNumericMetrics(path string) map[string]float64 {
	if path == "" {
		return nil
	}
	file, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer file.Close()
	metrics := make(map[string]float64)
	scanner := bufio.NewScanner(file)
	scanner.Buffer(make([]byte, 4096), 1024*1024)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		for len(line) > 0 {
			field := line
			if space := strings.IndexAny(line, " \t"); space >= 0 {
				field, line = line[:space], strings.TrimSpace(line[space:])
			} else {
				line = ""
			}
			equal := strings.IndexByte(field, '=')
			if equal <= 0 || equal == len(field)-1 {
				continue
			}
			key, raw := field[:equal], field[equal+1:]
			if !metricKey(key) {
				continue
			}
			value, err := strconv.ParseFloat(strings.TrimSuffix(raw, "ms"), 64)
			if err == nil {
				metrics[key] = value
			}
		}
	}
	return metrics
}

func metricKey(key string) bool {
	if key == "" {
		return false
	}
	for _, char := range key {
		if (char < 'a' || char > 'z') && (char < 'A' || char > 'Z') &&
			(char < '0' || char > '9') && char != '_' && char != '.' && char != '-' {
			return false
		}
	}
	return strings.HasSuffix(key, "_ms") || strings.HasSuffix(key, "_mib") ||
		strings.HasSuffix(key, "_bytes") || strings.HasSuffix(key, "_count") || key == "checksum"
}

func buildEnvironment(overrides []string) []string {
	if len(overrides) == 0 {
		return os.Environ()
	}
	type environmentValue struct {
		key   string
		value string
	}
	canonicalKey := func(key string) string {
		if runtime.GOOS == "windows" {
			return strings.ToUpper(key)
		}
		return key
	}
	values := make(map[string]environmentValue)
	order := make([]string, 0)
	for _, value := range os.Environ() {
		key, _, ok := strings.Cut(value, "=")
		if !ok {
			continue
		}
		canonical := canonicalKey(key)
		if _, exists := values[canonical]; !exists {
			order = append(order, canonical)
		}
		values[canonical] = environmentValue{key: key, value: value[len(key)+1:]}
	}
	for _, value := range overrides {
		key, raw, ok := strings.Cut(value, "=")
		if !ok || key == "" {
			continue
		}
		canonical := canonicalKey(key)
		if _, exists := values[canonical]; !exists {
			order = append(order, canonical)
		}
		values[canonical] = environmentValue{key: key, value: raw}
	}
	result := make([]string, 0, len(order))
	for _, canonical := range order {
		value := values[canonical]
		result = append(result, value.key+"="+value.value)
	}
	return result
}

func summarize(runs []runResult) runSummary {
	result := runSummary{Total: len(runs)}
	durations := make([]float64, 0, len(runs))
	for _, run := range runs {
		if run.Termination == "exited" && run.ExitCode == 0 {
			result.Successful++
			durations = append(durations, run.DurationMs)
		} else {
			result.Failed++
		}
		result.MaxPeakRSSBytes = max(result.MaxPeakRSSBytes, run.Peak.PeakRSS)
		result.MaxPeakPrivateBytes = max(result.MaxPeakPrivateBytes, run.Peak.PeakPrivate)
		result.MaxPeakVirtualBytes = max(result.MaxPeakVirtualBytes, run.Peak.PeakVirtual)
		result.MaxDurationMs = maxFloat(result.MaxDurationMs, run.DurationMs)
	}
	sort.Float64s(durations)
	if len(durations) > 0 {
		middle := len(durations) / 2
		if len(durations)%2 == 0 {
			result.MedianDurationMs = (durations[middle-1] + durations[middle]) / 2
		} else {
			result.MedianDurationMs = durations[middle]
		}
	}
	return result
}

func max(a, b uint64) uint64 {
	if a > b {
		return a
	}
	return b
}
func maxFloat(a, b float64) float64 {
	if a > b {
		return a
	}
	return b
}

var reportWriteMu sync.Mutex

func writeReport(path string, report runReport) error {
	encoded, err := json.MarshalIndent(report, "", "  ")
	if err != nil {
		return err
	}
	encoded = append(encoded, '\n')
	if path == "" {
		_, err = os.Stdout.Write(encoded)
		return err
	}
	reportWriteMu.Lock()
	defer reportWriteMu.Unlock()
	directory := filepath.Dir(path)
	if err := os.MkdirAll(directory, 0o755); err != nil {
		return err
	}
	temporary, err := os.CreateTemp(directory, ".limited-runner-*.json")
	if err != nil {
		return err
	}
	temporaryPath := temporary.Name()
	if _, err = temporary.Write(encoded); err == nil {
		err = temporary.Close()
	} else {
		_ = temporary.Close()
	}
	if err != nil {
		_ = os.Remove(temporaryPath)
		return err
	}
	return os.Rename(temporaryPath, path)
}

// fileSize and parseNumericMetrics intentionally read only after the child
// exits. During execution output is always streamed directly to disk.
