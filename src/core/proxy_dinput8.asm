; Export stubs for the dinput8.dll proxy: each jumps to the real System32 export, so no
; signatures are needed. Index order must match kNames in proxy_dinput8.cpp.
EXTERN g_dinput8_procs:QWORD

STUB MACRO name, idx
proxy_&name PROC
    jmp qword ptr [g_dinput8_procs + idx * 8]
proxy_&name ENDP
ENDM

.code
STUB DirectInput8Create, 0
STUB DllCanUnloadNow, 1
STUB DllGetClassObject, 2
STUB DllRegisterServer, 3
STUB DllUnregisterServer, 4
STUB GetdfDIJoystick, 5
END
