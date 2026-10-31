//go:build !windows

package main

import "os"

// POSIX rename does not care about open readers, so a plain open is enough.
func openShared(name string) (*os.File, error) { return os.Open(name) }
