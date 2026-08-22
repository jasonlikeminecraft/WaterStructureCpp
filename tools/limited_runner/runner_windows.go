//go:build windows

package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
)

var psapiGetProcessMemoryInfo = windows.NewLazySystemDLL("psapi.dll").NewProc("GetProcessMemoryInfo")

// PROCESS_MEMORY_COUNTERS_EX is declared locally because x/sys/windows keeps
// the PSAPI structure private. SIZE_T fields are uintptr on both x86 and x64.
type processMemoryCountersEx struct {
	CB                         uint32
	PageFaultCount             uint32
	PeakWorkingSetSize         uintptr
	WorkingSetSize             uintptr
	QuotaPeakPagedPoolUsage    uintptr
	QuotaPagedPoolUsage        uintptr
	QuotaPeakNonPagedPoolUsage uintptr
	QuotaNonPagedPoolUsage     uintptr
	PagefileUsage              uintptr
	PeakPagefileUsage          uintptr
	PrivateUsage               uintptr
}

// JOBOBJECT_BASIC_ACCOUNTING_INFORMATION is not currently exported by
// x/sys/windows. Job accounting includes active and already-terminated
// processes, so it is the reliable process-tree counter for a Job Object;
// GetProcessTimes on the root handle would omit worker subprocesses.
type jobObjectBasicAccountingInformation struct {
	TotalUserTime             int64
	TotalKernelTime           int64
	ThisPeriodTotalUserTime   int64
	ThisPeriodTotalKernelTime int64
	TotalPageFaultCount       uint32
	TotalProcesses            uint32
	ActiveProcesses           uint32
	TotalTerminatedProcesses  uint32
}

func platformName() string { return "windows-job-object" }

// Windows never uses the Unix helper mode. Keep a diagnostic implementation so
// an accidental invocation cannot silently run without a hard Job Object.
func internalChild(_ []string) int {
	fmt.Fprintln(os.Stderr, "internal child mode is unavailable on Windows")
	return 125
}

func startProcess(spec processSpec) (*processHandle, error) {
	stdout, err := openOutput(spec.stdoutPath)
	if err != nil {
		return nil, fmt.Errorf("open stdout: %w", err)
	}
	stderr, err := openOutput(spec.stderrPath)
	if err != nil {
		_ = stdout.Close()
		return nil, fmt.Errorf("open stderr: %w", err)
	}
	cleanupFiles := func() { _ = stdout.Close(); _ = stderr.Close() }

	job, err := windows.CreateJobObject(nil, nil)
	if err != nil {
		cleanupFiles()
		return nil, fmt.Errorf("create Job Object: %w", err)
	}
	closeJob := func() { _ = windows.CloseHandle(job) }
	limits := windows.JOBOBJECT_EXTENDED_LIMIT_INFORMATION{}
	limits.BasicLimitInformation.LimitFlags = windows.JOB_OBJECT_LIMIT_PROCESS_MEMORY |
		windows.JOB_OBJECT_LIMIT_JOB_MEMORY | windows.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
	limits.ProcessMemoryLimit = uintptr(spec.limitBytes)
	limits.JobMemoryLimit = uintptr(spec.limitBytes)
	if _, err = windows.SetInformationJobObject(job, windows.JobObjectExtendedLimitInformation,
		uintptr(unsafe.Pointer(&limits)), uint32(unsafe.Sizeof(limits))); err != nil {
		closeJob()
		cleanupFiles()
		return nil, fmt.Errorf("configure Job Object: %w", err)
	}

	resolved, err := exec.LookPath(spec.command)
	if err != nil {
		closeJob()
		cleanupFiles()
		return nil, fmt.Errorf("resolve command %q: %w", spec.command, err)
	}
	// CreateProcess cannot execute batch files directly. Match exec.Cmd's
	// behavior for the common .cmd/.bat case by invoking the command shell.
	commandArgs := append([]string{resolved}, spec.args...)
	application := resolved
	if ext := strings.ToLower(filepath.Ext(resolved)); ext == ".cmd" || ext == ".bat" {
		shell := os.Getenv("COMSPEC")
		if shell == "" {
			shell = `C:\Windows\System32\cmd.exe`
		}
		commandLine := windows.ComposeCommandLine(commandArgs)
		commandArgs = []string{shell, "/d", "/s", "/c", commandLine}
		application = shell
	}
	commandLine, err := windows.UTF16FromString(windows.ComposeCommandLine(commandArgs))
	if err != nil {
		closeJob()
		cleanupFiles()
		return nil, fmt.Errorf("encode command line: %w", err)
	}
	application16, err := windows.UTF16PtrFromString(application)
	if err != nil {
		closeJob()
		cleanupFiles()
		return nil, fmt.Errorf("encode executable: %w", err)
	}
	envValues := buildEnvironment(spec.env)
	envBlock, err := windowsEnvironmentBlock(envValues)
	if err != nil {
		closeJob()
		cleanupFiles()
		return nil, err
	}
	var cwd16 *uint16
	if spec.workingDir != "" {
		cwd16, err = windows.UTF16PtrFromString(spec.workingDir)
		if err != nil {
			closeJob()
			cleanupFiles()
			return nil, fmt.Errorf("encode working directory: %w", err)
		}
	}
	if err := windows.SetHandleInformation(windows.Handle(stdout.Fd()), windows.HANDLE_FLAG_INHERIT, windows.HANDLE_FLAG_INHERIT); err != nil {
		closeJob()
		cleanupFiles()
		return nil, fmt.Errorf("make stdout inheritable: %w", err)
	}
	if err := windows.SetHandleInformation(windows.Handle(stderr.Fd()), windows.HANDLE_FLAG_INHERIT, windows.HANDLE_FLAG_INHERIT); err != nil {
		closeJob()
		cleanupFiles()
		return nil, fmt.Errorf("make stderr inheritable: %w", err)
	}
	startup := windows.StartupInfo{}
	startup.Cb = uint32(unsafe.Sizeof(startup))
	startup.Flags = windows.STARTF_USESTDHANDLES
	startup.StdOutput = windows.Handle(stdout.Fd())
	startup.StdErr = windows.Handle(stderr.Fd())
	startup.StdInput = windows.Handle(0)
	info := windows.ProcessInformation{}
	flags := uint32(windows.CREATE_SUSPENDED | windows.CREATE_UNICODE_ENVIRONMENT | windows.CREATE_NO_WINDOW)
	if err := windows.CreateProcess(application16, &commandLine[0], nil, nil, true, flags,
		envBlock, cwd16, &startup, &info); err != nil {
		closeJob()
		cleanupFiles()
		return nil, fmt.Errorf("CreateProcess: %w", err)
	}
	childHandle := info.Process
	threadHandle := info.Thread
	failed := func(message string, cause error) (*processHandle, error) {
		_ = windows.TerminateProcess(childHandle, 125)
		_ = windows.CloseHandle(threadHandle)
		_ = windows.CloseHandle(childHandle)
		closeJob()
		cleanupFiles()
		if cause != nil {
			return nil, fmt.Errorf("%s: %w", message, cause)
		}
		return nil, errors.New(message)
	}
	if err := windows.AssignProcessToJobObject(job, childHandle); err != nil {
		return failed("assign process to Job Object", err)
	}
	if _, err := windows.ResumeThread(threadHandle); err != nil {
		return failed("resume process", err)
	}
	// The process has its own inherited references now. Closing the thread is
	// safe and avoids leaking one handle for every repeated benchmark.
	_ = windows.CloseHandle(threadHandle)
	done := make(chan processExit, 1)
	go func() {
		_, _ = windows.WaitForSingleObject(childHandle, windows.INFINITE)
		var code uint32
		err := windows.GetExitCodeProcess(childHandle, &code)
		exit := processExit{exitCode: int(code), err: err}
		if err == nil && code >= 0xC0000000 {
			exit.signal = fmt.Sprintf("windows_status_0x%08x", code)
		}
		done <- exit
	}()
	var closeOnce sync.Once
	return &processHandle{
		pid: int(info.ProcessId), backend: platformName(),
		hardTreeLimit: true, enforcement: "windows_job_object_process_and_job_memory",
		done: done,
		kill: func() error {
			err := windows.TerminateJobObject(job, 137)
			if err != nil {
				_ = windows.TerminateProcess(childHandle, 137)
			}
			return err
		},
		sample:       func() memorySample { return sampleWindowsProcess(childHandle, job) },
		sampleCPU:    func() cpuSample { return sampleWindowsJobCPU(job) },
		limitReached: func() bool { return windowsLimitReached(job, spec.limitBytes) },
		close: func() {
			closeOnce.Do(func() {
				_ = windows.CloseHandle(childHandle)
				closeJob()
				cleanupFiles()
			})
		},
	}, nil
}

func sampleWindowsJobCPU(job windows.Handle) cpuSample {
	accounting := jobObjectBasicAccountingInformation{}
	var returned uint32
	if err := windows.QueryInformationJobObject(job, windows.JobObjectBasicAccountingInformation,
		uintptr(unsafe.Pointer(&accounting)), uint32(unsafe.Sizeof(accounting)), &returned); err != nil {
		return cpuSample{Unavailable: "QueryInformationJobObject accounting: " + err.Error()}
	}
	// Job accounting durations are signed LARGE_INTEGER values measured in
	// 100-nanosecond ticks. Negative values are not valid for these counters.
	if accounting.TotalUserTime < 0 || accounting.TotalKernelTime < 0 {
		return cpuSample{Unavailable: "QueryInformationJobObject returned negative CPU time"}
	}
	userSeconds := float64(accounting.TotalUserTime) / 1e7
	kernelSeconds := float64(accounting.TotalKernelTime) / 1e7
	return cpuSample{
		UserSeconds:    userSeconds,
		SystemSeconds:  kernelSeconds,
		TotalSeconds:   userSeconds + kernelSeconds,
		SplitAvailable: true,
		Available:      true,
	}
}

func windowsLimitReached(job windows.Handle, limit uint64) bool {
	info := windows.JOBOBJECT_EXTENDED_LIMIT_INFORMATION{}
	var returned uint32
	if err := windows.QueryInformationJobObject(job, windows.JobObjectExtendedLimitInformation,
		uintptr(unsafe.Pointer(&info)), uint32(unsafe.Sizeof(info)), &returned); err != nil {
		return false
	}
	// PeakJobMemoryUsed is commit usage for the complete Job. A process memory
	// limit violation generally terminates before a sample can exceed the cap,
	// so treating an exact-cap peak as a hit preserves the reason in the report.
	return uint64(info.PeakJobMemoryUsed) >= limit
}

func windowsEnvironmentBlock(values []string) (*uint16, error) {
	// Windows requires environment entries sorted case-insensitively. Duplicate
	// keys have already been resolved by buildEnvironment.
	entries := append([]string(nil), values...)
	sort.SliceStable(entries, func(i, j int) bool { return strings.ToUpper(entries[i]) < strings.ToUpper(entries[j]) })
	block := make([]uint16, 0)
	for _, entry := range entries {
		encoded, err := windows.UTF16FromString(entry)
		if err != nil {
			return nil, fmt.Errorf("encode environment: %w", err)
		}
		block = append(block, encoded[:len(encoded)-1]...)
		block = append(block, 0)
	}
	block = append(block, 0)
	if len(block) == 0 {
		block = []uint16{0, 0}
	}
	return &block[0], nil
}

func sampleWindowsProcess(process windows.Handle, job windows.Handle) memorySample {
	result := memorySample{}
	counters := processMemoryCountersEx{CB: uint32(unsafe.Sizeof(processMemoryCountersEx{}))}
	if ret, _, _ := psapiGetProcessMemoryInfo.Call(uintptr(process), uintptr(unsafe.Pointer(&counters)), uintptr(unsafe.Sizeof(counters))); ret != 0 {
		result.CurrentRSS = uint64(counters.WorkingSetSize)
		result.PeakRSS = uint64(counters.PeakWorkingSetSize)
		result.CurrentPrivate = uint64(counters.PrivateUsage)
		result.PeakPrivate = uint64(counters.PeakPagefileUsage)
	}
	info := windows.JOBOBJECT_EXTENDED_LIMIT_INFORMATION{}
	var returned uint32
	if err := windows.QueryInformationJobObject(job, windows.JobObjectExtendedLimitInformation,
		uintptr(unsafe.Pointer(&info)), uint32(unsafe.Sizeof(info)), &returned); err == nil {
		if uint64(info.PeakJobMemoryUsed) > result.PeakPrivate {
			result.PeakPrivate = uint64(info.PeakJobMemoryUsed)
		}
	}
	if result.PeakRSS < result.CurrentRSS {
		result.PeakRSS = result.CurrentRSS
	}
	if result.PeakPrivate < result.CurrentPrivate {
		result.PeakPrivate = result.CurrentPrivate
	}
	return result
}

func openOutput(path string) (*os.File, error) {
	if path == "" {
		return nil, errors.New("empty output path")
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return nil, err
	}
	return os.OpenFile(path, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
}

var _ = runtime.GOOS
var _ = strconv.IntSize
var _ = time.Second
