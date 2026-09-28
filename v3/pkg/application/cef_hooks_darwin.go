//go:build darwin && !ios && !server

package application

var attachCEFDarwin = func(*macosWebviewWindow) {}
