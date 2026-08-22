//go:build !windows && (linux || android || darwin || freebsd || openbsd || netbsd || dragonfly)

package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

type cgroupInfo struct{ path string }

func platformName() string { return runtime.GOOS + "-prlimit" }

func internalChild(arguments []string) int {
	if len(arguments) < 2 {
		fmt.Fprintln(os.Stderr, "internal child: missing limit or command")
		return 64
	}
	limit, err := strconv.ParseUint(arguments[0], 10, 64)
	if err != nil || limit < 1 {
		fmt.Fprintln(os.Stderr, "internal child: invalid memory limit")
		return 64
	}
	commandIndex := 1
	gateFD := -1
	// The parent passes a close-on-exec gate as an inherited descriptor.  The
	// child waits before applying its per-process rlimit and exec, which removes
	// the target command's cgroup attach race. Keep the old two-argument form
	// usable for diagnostics.
	if len(arguments) > 2 && strings.HasPrefix(arguments[1], "--gate-fd=") {
		parsed, parseErr := strconv.Atoi(strings.TrimPrefix(arguments[1], "--gate-fd="))
		if parseErr != nil || parsed < 0 {
			fmt.Fprintln(os.Stderr, "internal child: invalid gate fd")
			return 64
		}
		gateFD = parsed
		commandIndex = 2
	}
	command := arguments[commandIndex]
	childArgs := append([]string{command}, arguments[commandIndex+1:]...)
	if gateFD >= 0 {
		gate := os.NewFile(uintptr(gateFD), "limited-runner-gate")
		if gate == nil {
			fmt.Fprintln(os.Stderr, "internal child: create gate file")
			return 125
		}
		var token [1]byte
		_, readErr := gate.Read(token[:])
		_ = gate.Close()
		if readErr != nil {
			fmt.Fprintf(os.Stderr, "internal child: wait for parent gate: %v\n", readErr)
			return 125
		}
	}
	if err := setProcessMemoryLimit(limit); err != nil {
		fmt.Fprintf(os.Stderr, "internal child: set %s: %v\n", processMemoryLimitName, err)
		return 125
	}
	resolved, err := exec.LookPath(command)
	if err != nil {
		fmt.Fprintf(os.Stderr, "internal child: resolve %q: %v\n", command, err)
		return 127
	}
	if err := syscall.Exec(resolved, childArgs, os.Environ()); err != nil {
		fmt.Fprintf(os.Stderr, "internal child: exec %q: %v\n", resolved, err)
		return 127
	}
	return 127
}

func startProcess(spec processSpec) (*processHandle, error) {
	if err := validateProcessMonitor(); err != nil {
		return nil, fmt.Errorf("initialize process-tree monitor: %w", err)
	}
	stdout, err := openOutput(spec.stdoutPath)
	if err != nil {
		return nil, fmt.Errorf("open stdout: %w", err)
	}
	stderr, err := openOutput(spec.stderrPath)
	if err != nil {
		_ = stdout.Close()
		return nil, fmt.Errorf("open stderr: %w", err)
	}

	helper, err := os.Executable()
	if err != nil {
		_ = stdout.Close()
		_ = stderr.Close()
		return nil, fmt.Errorf("locate runner executable: %w", err)
	}
	// Keep the helper stopped at a small gate until it has been moved into the
	// private cgroup.  Without this handshake a fast child can allocate (or
	// spawn descendants) before cgroup.procs is written, defeating the hard
	// process-tree limit.
	gateReader, gateWriter, err := os.Pipe()
	if err != nil {
		_ = stdout.Close()
		_ = stderr.Close()
		return nil, fmt.Errorf("create cgroup gate: %w", err)
	}
	args := []string{"--internal-child", strconv.FormatUint(spec.limitBytes, 10), "--gate-fd=3", spec.command}
	args = append(args, spec.args...)
	cmd := exec.Command(helper, args...)
	cmd.Dir = spec.workingDir
	cmd.Env = buildEnvironment(spec.env)
	cmd.Stdout, cmd.Stderr = stdout, stderr
	cmd.ExtraFiles = []*os.File{gateReader}
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	if err := cmd.Start(); err != nil {
		_ = gateReader.Close()
		_ = gateWriter.Close()
		_ = stdout.Close()
		_ = stderr.Close()
		return nil, fmt.Errorf("start child: %w", err)
	}
	_ = gateReader.Close()
	cgroup, cgroupErr := createCgroup(cmd.Process.Pid, spec.limitBytes)
	// Even when cgroup setup is unavailable, release the child so the parent
	// monitor/rlimit fallback can take over. Closing the writer also unblocks a
	// child that exits while setup is in progress.
	if _, writeErr := gateWriter.Write([]byte{1}); writeErr != nil {
		_ = gateWriter.Close()
		_ = killUnixTree(cmd.Process.Pid, cgroup)
		_, _ = cmd.Process.Wait()
		_ = stdout.Close()
		_ = stderr.Close()
		if cgroup != nil {
			_ = waitCgroupEmpty(cgroup, 500*time.Millisecond)
			_ = removeCgroup(cgroup)
		}
		return nil, fmt.Errorf("release cgroup gate: %w", writeErr)
	}
	_ = gateWriter.Close()
	backend := platformName()
	if cgroup != nil {
		backend += "+cgroupv2"
	}
	if cgroupErr != nil {
		backend += "+monitor"
	}
	done := make(chan processExit, 1)
	go func() {
		err := cmd.Wait()
		exit := processExit{}
		if state, ok := cmd.ProcessState.Sys().(syscall.WaitStatus); ok {
			if state.Signaled() {
				exit.signal = state.Signal().String()
				exit.exitCode = -1
			} else {
				exit.exitCode = state.ExitStatus()
			}
		} else if cmd.ProcessState != nil {
			exit.exitCode = cmd.ProcessState.ExitCode()
		}
		if _, normalExit := err.(*exec.ExitError); !normalExit {
			exit.err = err
		}
		done <- exit
	}()
	var closeOnce sync.Once
	var trackedMu sync.Mutex
	tracked := map[int]struct{}{cmd.Process.Pid: {}}
	trackDescendants := func() {
		for _, entry := range processTree(cmd.Process.Pid) {
			trackedMu.Lock()
			tracked[entry.pid] = struct{}{}
			trackedMu.Unlock()
		}
	}
	killTracked := func() error {
		trackDescendants()
		firstErr := killUnixTree(cmd.Process.Pid, cgroup)
		trackedMu.Lock()
		pids := make([]int, 0, len(tracked))
		for pid := range tracked {
			pids = append(pids, pid)
		}
		trackedMu.Unlock()
		for _, pid := range pids {
			if err := syscall.Kill(pid, syscall.SIGKILL); err != nil && !errors.Is(err, syscall.ESRCH) && firstErr == nil {
				firstErr = err
			}
		}
		return firstErr
	}
	return &processHandle{
		pid: cmd.Process.Pid, backend: backend,
		hardTreeLimit: cgroup != nil,
		enforcement: func() string {
			if cgroup != nil {
				return "cgroup_v2_memory_max+" + processMemoryLimitEnforcement
			}
			return processMemoryLimitEnforcement + "+sampled_process_tree_monitor"
		}(),
		done: done,
		kill: killTracked,
		sample: func() memorySample {
			trackDescendants()
			return sampleUnixProcess(cmd.Process.Pid, cgroup)
		},
		sampleCPU: func() cpuSample {
			trackDescendants()
			return sampleUnixCPU(cmd.Process.Pid, cgroup)
		},
		limitReached: func() bool { return unixLimitReached(cmd.Process.Pid, cgroup, spec.limitBytes) },
		close: func() {
			closeOnce.Do(func() {
				_ = stdout.Close()
				_ = stderr.Close()
				// A root process can exit while descendants remain. Clean those
				// descendants before removing the cgroup; otherwise cgroup removal
				// fails and leaves a leaked directory/process behind.
				_ = killTracked()
				if cgroup != nil {
					for attempt := 0; attempt < 3; attempt++ {
						if waitCgroupEmpty(cgroup, 500*time.Millisecond) == nil {
							if removeCgroup(cgroup) == nil {
								break
							}
						}
						_ = killTracked()
					}
				}
			})
		},
	}, nil
}

func unixLimitReached(pid int, group *cgroupInfo, limit uint64) bool {
	if group != nil {
		if peak := readUintFile(filepath.Join(group.path, "memory.peak")); peak >= limit {
			return true
		}
		if raw, err := os.ReadFile(filepath.Join(group.path, "memory.events")); err == nil {
			for _, line := range strings.Split(string(raw), "\n") {
				parts := strings.Fields(line)
				if len(parts) == 2 && (parts[0] == "oom" || parts[0] == "oom_kill") {
					value, _ := strconv.ParseUint(parts[1], 10, 64)
					if value > 0 {
						return true
					}
				}
			}
		}
	}
	sample := sampleUnixProcess(pid, group)
	return sample.CurrentVirtual >= limit || sample.CurrentRSS >= limit || sample.CurrentPrivate >= limit
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

func createCgroup(pid int, limit uint64) (*cgroupInfo, error) {
	// cgroup v2 is optional on Linux/Termux.  Failure is intentionally not
	// fatal because the child has RLIMIT_AS and is still sampled/killed by the
	// parent.  Never touch the caller's own cgroup; create one child directory.
	if runtime.GOOS != "linux" && runtime.GOOS != "android" {
		return nil, errors.New("cgroup v2 unavailable")
	}
	root := "/sys/fs/cgroup"
	if _, err := os.Stat(filepath.Join(root, "cgroup.controllers")); err != nil {
		return nil, err
	}
	base := root
	if raw, err := os.ReadFile("/proc/self/cgroup"); err == nil {
		for _, line := range strings.Split(string(raw), "\n") {
			if strings.HasPrefix(line, "0::") {
				path := strings.TrimSpace(strings.TrimPrefix(line, "0::"))
				path = strings.TrimPrefix(path, "/")
				if path != "" {
					base = filepath.Join(root, path)
				}
				break
			}
		}
	}
	name := fmt.Sprintf("waterstructure-limited-%d-%d", pid, time.Now().UnixNano())
	directory := filepath.Join(base, name)
	if err := os.Mkdir(directory, 0o755); err != nil {
		return nil, err
	}
	cleanup := func() { _ = os.Remove(directory) }
	if err := os.WriteFile(filepath.Join(directory, "memory.max"), []byte(strconv.FormatUint(limit, 10)), 0o644); err != nil {
		cleanup()
		return nil, err
	}
	// Swap is best effort: some hosts expose memory.swap.max read-only.
	_ = os.WriteFile(filepath.Join(directory, "memory.swap.max"), []byte("0"), 0o644)
	if err := os.WriteFile(filepath.Join(directory, "cgroup.procs"), []byte(strconv.Itoa(pid)), 0o644); err != nil {
		cleanup()
		return nil, err
	}
	return &cgroupInfo{path: directory}, nil
}

func removeCgroup(group *cgroupInfo) error {
	if group == nil {
		return nil
	}
	return os.Remove(group.path)
}

func cgroupPIDs(group *cgroupInfo) []int {
	if group == nil {
		return nil
	}
	raw, err := os.ReadFile(filepath.Join(group.path, "cgroup.procs"))
	if err != nil {
		return nil
	}
	result := make([]int, 0)
	for _, line := range strings.Fields(string(raw)) {
		if pid, parseErr := strconv.Atoi(line); parseErr == nil && pid > 0 {
			result = append(result, pid)
		}
	}
	return result
}

func killUnixTree(root int, group *cgroupInfo) error {
	var firstErr error
	// cgroup.kill is atomic for the complete cgroup and avoids a race where a
	// descendant forks while the /proc list is being traversed.
	if group != nil {
		if err := os.WriteFile(filepath.Join(group.path, "cgroup.kill"), []byte("1"), 0o644); err != nil && !errors.Is(err, os.ErrNotExist) {
			firstErr = err
		}
	}
	// Also kill the process group: this covers kernels without cgroup.kill and
	// descendants that escaped before cgroup attachment.
	if err := syscall.Kill(-root, syscall.SIGKILL); err != nil && !errors.Is(err, syscall.ESRCH) {
		if firstErr == nil {
			firstErr = err
		}
	}
	for _, pid := range cgroupPIDs(group) {
		if err := syscall.Kill(pid, syscall.SIGKILL); err != nil && !errors.Is(err, syscall.ESRCH) && firstErr == nil {
			firstErr = err
		}
	}
	// Last-resort recursive /proc traversal for the no-cgroup fallback.
	for _, entry := range processTree(root) {
		if err := syscall.Kill(entry.pid, syscall.SIGKILL); err != nil && !errors.Is(err, syscall.ESRCH) && firstErr == nil {
			firstErr = err
		}
	}
	return firstErr
}

func waitCgroupEmpty(group *cgroupInfo, timeout time.Duration) error {
	if group == nil {
		return nil
	}
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if len(cgroupPIDs(group)) == 0 {
			return nil
		}
		time.Sleep(10 * time.Millisecond)
	}
	if len(cgroupPIDs(group)) != 0 {
		return errors.New("cgroup still contains processes")
	}
	return nil
}

func sampleUnixCPU(pid int, group *cgroupInfo) cpuSample {
	if group != nil {
		if raw, err := os.ReadFile(filepath.Join(group.path, "cpu.stat")); err == nil {
			var userMicros, systemMicros uint64
			var totalMicros uint64
			parsedStat := false
			userSeen := false
			systemSeen := false
			for _, line := range strings.Split(string(raw), "\n") {
				fields := strings.Fields(line)
				if len(fields) != 2 {
					continue
				}
				value, parseErr := strconv.ParseUint(fields[1], 10, 64)
				if parseErr != nil {
					continue
				}
				switch fields[0] {
				case "user_usec":
					parsedStat = true
					userSeen = true
					userMicros = value
				case "system_usec":
					parsedStat = true
					systemSeen = true
					systemMicros = value
				case "usage_usec":
					parsedStat = true
					totalMicros = value
				}
			}
			if parsedStat {
				// Kernels predating user_usec/system_usec expose only total
				// usage. Preserve that distinction instead of presenting total
				// CPU as measured user time.
				if totalMicros == 0 {
					totalMicros = userMicros + systemMicros
				}
				return cpuSample{
					UserSeconds:    float64(userMicros) / 1e6,
					SystemSeconds:  float64(systemMicros) / 1e6,
					TotalSeconds:   float64(totalMicros) / 1e6,
					SplitAvailable: userSeen && systemSeen,
					Available:      true,
				}
			}
		}
	}
	return sampleProcessTreeCPU(pid)
}

func sampleUnixProcess(pid int, group *cgroupInfo) memorySample {
	result := sampleProcessTreeMemory(pid)
	if group != nil {
		current := readUintFile(filepath.Join(group.path, "memory.current"))
		peak := readUintFile(filepath.Join(group.path, "memory.peak"))
		if current > result.CurrentRSS {
			result.CurrentRSS = current
		}
		if peak > result.PeakRSS {
			result.PeakRSS = peak
		}
	}
	return normalizeMemory(result)
}

func addMemory(dst *memorySample, value memorySample) {
	dst.CurrentRSS += value.CurrentRSS
	dst.PeakRSS += value.PeakRSS
	dst.CurrentPrivate += value.CurrentPrivate
	dst.PeakPrivate += value.PeakPrivate
	dst.CurrentVirtual += value.CurrentVirtual
	dst.PeakVirtual += value.PeakVirtual
}

func normalizeMemory(result memorySample) memorySample {
	if result.PeakRSS < result.CurrentRSS {
		result.PeakRSS = result.CurrentRSS
	}
	if result.PeakVirtual < result.CurrentVirtual {
		result.PeakVirtual = result.CurrentVirtual
	}
	if result.PeakPrivate < result.CurrentPrivate {
		result.PeakPrivate = result.CurrentPrivate
	}
	return result
}

func readUintFile(path string) uint64 {
	data, err := os.ReadFile(path)
	if err != nil {
		return 0
	}
	value, _ := strconv.ParseUint(strings.TrimSpace(string(data)), 10, 64)
	return value
}
