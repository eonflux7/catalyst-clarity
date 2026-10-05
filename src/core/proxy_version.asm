; Export stubs for the version.dll proxy: each jumps to the real System32 export, so no
; signatures are needed. Index order must match kNames in proxy_version.cpp.
EXTERN g_version_procs:QWORD

STUB MACRO name, idx
proxy_&name PROC
    jmp qword ptr [g_version_procs + idx * 8]
proxy_&name ENDP
ENDM

.code
STUB GetFileVersionInfoA, 0
STUB GetFileVersionInfoByHandle, 1
STUB GetFileVersionInfoExA, 2
STUB GetFileVersionInfoExW, 3
STUB GetFileVersionInfoSizeA, 4
STUB GetFileVersionInfoSizeExA, 5
STUB GetFileVersionInfoSizeExW, 6
STUB GetFileVersionInfoSizeW, 7
STUB GetFileVersionInfoW, 8
STUB VerFindFileA, 9
STUB VerFindFileW, 10
STUB VerInstallFileA, 11
STUB VerInstallFileW, 12
STUB VerLanguageNameA, 13
STUB VerLanguageNameW, 14
STUB VerQueryValueA, 15
STUB VerQueryValueW, 16
END
