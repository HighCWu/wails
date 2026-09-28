//go:build windows && cgo && wails_cef

package main

/*
#include <windows.h>
#include <stdio.h>
static LONG CALLBACK smoke_exception(EXCEPTION_POINTERS *info) {
 if(info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
 fprintf(stderr, "CEF_SMOKE_EXCEPTION %lx at %p\n", info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
 void *frames[64]; USHORT n=CaptureStackBackTrace(0,64,frames,NULL);
 for(USHORT i=0;i<n;i++) {
  MEMORY_BASIC_INFORMATION m; char name[MAX_PATH]={0};
  if(VirtualQuery(frames[i],&m,sizeof(m))) {
   GetModuleFileNameA((HMODULE)m.AllocationBase,name,MAX_PATH);
   fprintf(stderr,"CEF_SMOKE_STACK %s+0x%llx\n",name,(unsigned long long)((char*)frames[i]-(char*)m.AllocationBase));
  }
 }
 fflush(stderr);
 return EXCEPTION_CONTINUE_SEARCH;
}
static void smoke_install_exception_trace(void) { AddVectoredExceptionHandler(0,smoke_exception); }
*/
import "C"

import "os"

func init() {
	if os.Getenv("CEF_SMOKE_TRACE_CRASH") == "1" {
		C.smoke_install_exception_trace()
	}
}
