//go:build windows

package main

import (
	"fmt"
	"unsafe"

	"golang.org/x/sys/windows"
)

// Keep the job handle open until process exit. Closing a job configured with
// KILL_ON_JOB_CLOSE would terminate the process immediately.
var manifestMemoryJob windows.Handle

func installProcessMemoryLimit(limit uint64) error {
	job, err := windows.CreateJobObject(nil, nil)
	if err != nil {
		return fmt.Errorf("create memory-limit job: %w", err)
	}
	info := windows.JOBOBJECT_EXTENDED_LIMIT_INFORMATION{}
	info.BasicLimitInformation.LimitFlags =
		windows.JOB_OBJECT_LIMIT_PROCESS_MEMORY | windows.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
	info.ProcessMemoryLimit = uintptr(limit)
	if _, err := windows.SetInformationJobObject(
		job,
		windows.JobObjectExtendedLimitInformation,
		uintptr(unsafe.Pointer(&info)),
		uint32(unsafe.Sizeof(info)),
	); err != nil {
		_ = windows.CloseHandle(job)
		return fmt.Errorf("set memory-limit job: %w", err)
	}
	if err := windows.AssignProcessToJobObject(job, windows.CurrentProcess()); err != nil {
		_ = windows.CloseHandle(job)
		// A parent job may forbid nested jobs. The Go heap limit remains active,
		// so callers running under such a supervisor can still proceed.
		return nil
	}
	manifestMemoryJob = job
	return nil
}
