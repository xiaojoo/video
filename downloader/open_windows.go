//go:build windows

package main

import (
	"os"
	"syscall"
)

// openShared opens for reading with FILE_SHARE_DELETE, which os.Open does not ask
// for. Without it a rename of the file fails while any reader holds it, and the
// reader is exactly what a finishing download is still serving.
func openShared(name string) (*os.File, error) {
	p, err := syscall.UTF16PtrFromString(name)
	if err != nil {
		return nil, err
	}
	h, err := syscall.CreateFile(p, syscall.GENERIC_READ,
		syscall.FILE_SHARE_READ|syscall.FILE_SHARE_WRITE|syscall.FILE_SHARE_DELETE,
		nil, syscall.OPEN_EXISTING, syscall.FILE_ATTRIBUTE_NORMAL, 0)
	if err != nil {
		return nil, err
	}
	return os.NewFile(uintptr(h), name), nil
}
