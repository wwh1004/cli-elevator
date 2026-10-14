/*
* cli-elevator - shared declarations
*
* Two modes, selected by the first argument:
*   a) a raw command line  -> launcher mode, creates the pipes and elevates
*      a copy of this exe with the bypass below.
*   b) \\.\pipe\xxxxx      -> broker mode, runs elevated, spawns the target.
*/

#ifndef CLI_ELEVATOR_H
#define CLI_ELEVATOR_H

#include <windows.h>
#include <winternl.h>       /* NTSTATUS, PROCESSINFOCLASS */
#include <rpc.h>
#include <rpcndr.h>
#include <rpcasync.h>

/*
* STATUS codes / macros. Deliberately hand rolled so that neither ntstatus.h
* nor the WIN32_NO_STATUS + windows.h dance is needed.
*/
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_UNSUCCESSFUL
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001L)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#endif
#ifndef STATUS_ACCESS_DENIED
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022L)
#endif
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

/*
* PROCESSINFOCLASS value used to grab a process debug object handle.
* winternl.h stops at ProcessBreakOnTermination (29).
*/
#define ProcessDebugObjectHandle ((PROCESSINFOCLASS)30)

/*
* ntdll exports that winternl.h does not declare. Linked against ntdll.lib.
*/
#ifndef NtCurrentProcess
#define NtCurrentProcess() ((HANDLE)(LONG_PTR)-1)
#endif

NTSYSAPI NTSTATUS NTAPI NtDuplicateObject(
    HANDLE SourceProcessHandle,
    HANDLE SourceHandle,
    HANDLE TargetProcessHandle,
    PHANDLE TargetHandle,
    ACCESS_MASK DesiredAccess,
    ULONG HandleAttributes,
    ULONG Options);

NTSYSAPI NTSTATUS NTAPI NtRemoveProcessDebug(
    HANDLE ProcessHandle,
    HANDLE DebugObjectHandle);

NTSYSAPI VOID NTAPI DbgUiSetThreadDebugObject(
    HANDLE DebugObject);

/*
* Pipe layout. The launcher owns the server ends of all three pipes,
* the broker connects to them as a client.
*
*   <base>-stdin   launcher -> broker : framed command line, then ctrl bytes
*   <base>-stdout  broker   -> launcher : target stdout
*   <base>-stderr  broker   -> launcher : target stderr
*/
#define CLI_PIPE_PREFIX     L"\\\\.\\pipe\\"
#define CLI_PIPE_STDIN      L"-stdin"
#define CLI_PIPE_STDOUT     L"-stdout"
#define CLI_PIPE_STDERR     L"-stderr"
#define CLI_PIPE_BUF_SIZE   (64 * 1024)

/* byte the launcher pushes into <base>-stdin when the user hits Ctrl+C */
#define CLI_CTRL_C_BYTE     0x03
/* guard for the framed command line, protocol sanity check */
#define CLI_MAX_CMDLINE     0x10000

/* uacme.c - UAC bypass, see file header */
BOOL CliElevateAndLaunch(
    LPCWSTR BrokerCommandLine,
    PROCESS_INFORMATION *Payload);

/* util.c */
BOOL CliReadAll(
    HANDLE Handle,
    VOID *Buffer,
    DWORD Length);

BOOL CliWriteAll(
    HANDLE Handle,
    const VOID *Buffer,
    DWORD Length);

BOOL CliHandleUsable(
    HANDLE Handle);

VOID CliTrace(
    LPCWSTR Format,
    ...);

BOOL CliStopThread(
    HANDLE hThread,
    DWORD TimeoutMs);

LPCWSTR CliSkipProgramName(
    LPCWSTR CommandLine);

BOOL CliFirstToken(
    LPCWSTR CommandLine,
    LPWSTR Buffer,
    SIZE_T cchBuffer);

BOOL CliIsPathRooted(
    LPCWSTR Path);

BOOL CliCopyString(
    LPWSTR Buffer,
    SIZE_T cchBuffer,
    LPCWSTR Source);

BOOL CliCombinePath(
    LPCWSTR Base,
    LPCWSTR Name,
    LPWSTR Buffer,
    SIZE_T cchBuffer);

/* main.c - mode a */
int CliRunLauncher(
    LPCWSTR RawCommandLine);

/* broker.c - mode b */
int CliRunBroker(
    LPCWSTR PipeBaseName);

#endif /* CLI_ELEVATOR_H */
