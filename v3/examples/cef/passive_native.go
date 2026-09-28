//go:build windows || (darwin && !ios)

package main

func configurePassiveWindows() { panic("Passive desktop testing is only available on Linux; use native_smoke.py on a CI runner") }
