/*******************************************************************************
*
*  cli-elevator - launcher (mode a) and program entry point.
*
*  Mode a: the user types
*
*      cli-elevator <raw command line>
*
*  This process creates three named pipes, elevates a copy of itself as the
*  broker (UAC bypass, see uacme.c), hands the raw command line to the broker
*  over <base>-stdin and then relays stdio between this console and the broker
*  until the broker exits.
*
*  Mode b: the elevated broker, invoked as "cli-elevator \\.\pipe\<base>",
*  lives in broker.c.
*
*******************************************************************************/

#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#include "cli-elevator.h"

/* <base>-stdin, and it is owned by the launcher */
static HANDLE g_hPipeIn = NULL;
/* set once the framed command line has been written to g_hPipeIn */
static volatile LONG g_ForwardReady = 0;

/*
* CliError
*
* Purpose:
*
* Report a launcher side failure on stderr.
*
*/
static VOID CliError(
    LPCWSTR Format,
    ...)
{
    WCHAR szBuffer[1024];
    va_list args;

    va_start(args, Format);
    _vsnwprintf_s(szBuffer, ARRAYSIZE(szBuffer), _TRUNCATE, Format, args);
    va_end(args);

    fwprintf(stderr, L"cli-elevator: %s\n", szBuffer);
    fflush(stderr);
}

/*
* CliIsPipeName
*
* Purpose:
*
* TRUE if the token addresses a named pipe, i.e. this is broker mode.
*
*/
static BOOL CliIsPipeName(
    LPCWSTR Token)
{
    if (_wcsnicmp(Token, L"\\\\.\\pipe\\", 9) == 0)
        return TRUE;
    if (_wcsnicmp(Token, L"\\\\?\\pipe\\", 8) == 0)
        return TRUE;
    return FALSE;
}

/*
* CliCtrlHandler
*
* Purpose:
*
* Console control handler. Ctrl+C / Ctrl+Break never reach the target process
* directly, the launcher pushes a 0x03 byte into <base>-stdin and the broker
* turns it into a control event for the target.
*
*/
static BOOL WINAPI CliCtrlHandler(
    DWORD CtrlType)
{
    DWORD written = 0;
    BYTE ctrl = CLI_CTRL_C_BYTE;

    switch (CtrlType) {

    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        CliTrace(L"launcher: console ctrl event %u", (unsigned)CtrlType);

        if (InterlockedCompareExchange(&g_ForwardReady, 1, 1) == 1) {
            if (g_hPipeIn && g_hPipeIn != INVALID_HANDLE_VALUE) {
                if (!WriteFile(g_hPipeIn, &ctrl, sizeof(ctrl), &written, NULL))
                    CliTrace(L"launcher: forwarding ctrl-c failed, error %lu", GetLastError());
            }
        }
        else {
            CliTrace(L"launcher: ctrl event before the handshake, dropped");
        }
        // swallow it, the launcher outlives the target
        return TRUE;

    default:
        return FALSE;
    }
}

/*
* Relay context / threads
*
*/
typedef struct _RELAY_CTX {
    HANDLE Source;
    HANDLE Sink;
} RELAY_CTX;

typedef struct _CONNECT_CTX {
    HANDLE Pipes[3];
    HANDLE DoneEvent;
    LONG Failed;
} CONNECT_CTX;

/*
* CliRelayThread
*
* Purpose:
*
* Pump everything from Source to Sink until EOF.
*
*/
static DWORD WINAPI CliRelayThread(
    LPVOID Parameter)
{
    RELAY_CTX *ctx = (RELAY_CTX *)Parameter;
    BYTE buffer[8192];
    DWORD read = 0;

    for (;;) {
        if (!ReadFile(ctx->Source, buffer, sizeof(buffer), &read, NULL) || read == 0)
            break;
        if (!CliWriteAll(ctx->Sink, buffer, read))
            break;
    }

    return 0;
}

/*
* CliStdinThread
*
* Purpose:
*
* Forward whatever is typed in this console to the broker, which relays it to the
* target stdin. Ctrl+C is not seen here (it is a console control event), it goes
* through CliCtrlHandler.
*
* A pending console read is cancelled when the user hits Ctrl+C, so that the
* control handler gets to run: ReadFile then fails with ERROR_OPERATION_ABORTED.
* That is not the end of the console session - treating it as one silently kills
* input forwarding for the rest of the run.
*
*/
static DWORD WINAPI CliStdinThread(
    LPVOID Parameter)
{
    HANDLE hPipeIn = (HANDLE)Parameter;
    HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
    BYTE buffer[4096];
    DWORD read = 0;
    DWORD error;
    DWORD mode = 0;
    DWORD retries = 0;
    BOOL isConsole;

    if (!CliHandleUsable(hStdIn))
        return 0;

    isConsole = (GetConsoleMode(hStdIn, &mode) != FALSE);

    for (;;) {

        if (ReadFile(hStdIn, buffer, sizeof(buffer), &read, NULL) && read > 0) {
            retries = 0;
            if (!CliWriteAll(hPipeIn, buffer, read))
                break;
            continue;
        }

        error = GetLastError();

        if (isConsole) {
            //
            // Console input, the Ctrl+C cancellation above is expected here and
            // by far the most common reason to land in this branch. Keep the
            // loop alive no matter what the console reports, there is nothing
            // useful to do with a console that somehow went bad anyway.
            //
            if (retries++ == 0)
                CliTrace(L"launcher: stdin read error %lu, keep listening", error);

            Sleep(5);
            continue;
        }

        //
        // Redirected input: a zero byte read really is the end.
        //
        if (read == 0)
            CliTrace(L"launcher: stdin at end of input, forwarding stops");
        else
            CliTrace(L"launcher: stdin read failed, forwarding stops (error %lu)", error);
        break;
    }

    return 0;
}

/*
* CliConnectThread
*
* Purpose:
*
* Wait for the elevated broker to connect to all three pipe instances.
*
*/
static DWORD WINAPI CliConnectThread(
    LPVOID Parameter)
{
    CONNECT_CTX *ctx = (CONNECT_CTX *)Parameter;
    INT i;

    for (i = 0; i < 3; i++) {
        if (!ConnectNamedPipe(ctx->Pipes[i], NULL)) {
            if (GetLastError() != ERROR_PIPE_CONNECTED) {
                InterlockedExchange(&ctx->Failed, 1);
                break;
            }
        }
    }

    SetEvent(ctx->DoneEvent);
    return 0;
}

/*
* CliNoElevateRequested
*
* Purpose:
*
* Test hook. With CLI_ELEVATOR_NO_ELEVATE=1 the broker is started directly
* instead of through the UAC bypass, which makes the pipe protocol and the
* relay testable without touching UAC at all.
*
*/
static BOOL CliNoElevateRequested(
    VOID)
{
    WCHAR szValue[8];

    return (GetEnvironmentVariableW(L"CLI_ELEVATOR_NO_ELEVATE", szValue, ARRAYSIZE(szValue)) > 0)
        && (szValue[0] != L'0');
}

/*
* CliCreateKillJob
*
* Purpose:
*
* A job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE that this process joins,
* so that the broker it starts inherits it and the target the broker starts
* inherits it in turn.
*
* The point is what happens when this process does not get to run Cleanup:
* killed from the task manager, console window closed, launcher crashed. The
* kernel closes the last handle to the job on the way out and everything still
* in it - broker, target, anything the target started - is terminated with it.
* The alternative, killing the broker from Cleanup, only covers orderly exits.
*
* Returns the job handle, or NULL when the process is already inside a job that
* does not nest, in which case the caller carries on without the safety net.
* The handle is deliberately never closed: letting the process exit close it is
* the trigger we want.
*
*/
static HANDLE CliCreateKillJob(
    BOOL *JoinedBySelf)
{
    HANDLE hJob;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;

    *JoinedBySelf = FALSE;

    hJob = CreateJobObjectW(NULL, NULL);
    if (hJob == NULL) {
        CliTrace(L"launcher: CreateJobObject failed, error %lu", GetLastError());
        return NULL;
    }

    RtlSecureZeroMemory(&info, sizeof(info));
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation,
        &info, sizeof(info)))
    {
        CliTrace(L"launcher: SetInformationJobObject failed, error %lu", GetLastError());
        CloseHandle(hJob);
        return NULL;
    }

    if (AssignProcessToJobObject(hJob, GetCurrentProcess())) {
        *JoinedBySelf = TRUE;
        CliTrace(L"launcher: kill job armed, this process joined it");
    }
    else {
        CliTrace(L"launcher: cannot join our own job (error %lu), "
            L"will assign the broker instead", GetLastError());
    }

    return hJob;
}

/*
* CliRunLauncher
*
* Purpose:
*
* Mode a, see file header.
*
*/
int CliRunLauncher(
    LPCWSTR RawCommandLine)
{
    WCHAR szExe[MAX_PATH * 2];
    WCHAR szBase[MAX_PATH];
    WCHAR szPipeIn[MAX_PATH];
    WCHAR szPipeOut[MAX_PATH];
    WCHAR szPipeErr[MAX_PATH];
    WCHAR szBrokerCmdLine[MAX_PATH * 4];
    WCHAR szCurrentDirectory[MAX_PATH * 2];
    HANDLE hIn = INVALID_HANDLE_VALUE;
    HANDLE hOut = INVALID_HANDLE_VALUE;
    HANDLE hErr = INVALID_HANDLE_VALUE;
    HANDLE hConnected = NULL;
    HANDLE hThreads[3] = { NULL, NULL, NULL };
    PROCESS_INFORMATION payload;
    CONNECT_CTX connectCtx;
    RELAY_CTX outRelay, errRelay;
    HANDLE hKillJob = NULL;
    BOOL joinedBySelf = FALSE;
    DWORD cbCommandLine, cbCurrentDirectory, exitCode = 0;
    LARGE_INTEGER counter;
    int result = 1;

    RtlSecureZeroMemory(&payload, sizeof(payload));
    RtlSecureZeroMemory(&connectCtx, sizeof(connectCtx));
    RtlSecureZeroMemory(&outRelay, sizeof(outRelay));
    RtlSecureZeroMemory(&errRelay, sizeof(errRelay));

    //
    // Safety net first: everything started from here on ends up in this job, so
    // it dies with this process even if this process never gets to clean up.
    // Handle intentionally not closed, see CliCreateKillJob.
    //
    hKillJob = CliCreateKillJob(&joinedBySelf);

    //
    // Unique per launcher run pipe base name.
    //
    QueryPerformanceCounter(&counter);
    _snwprintf_s(szBase, ARRAYSIZE(szBase), _TRUNCATE,
        L"%scli-elevator-%u-%08X%08X",
        CLI_PIPE_PREFIX,
        (unsigned)GetCurrentProcessId(),
        (unsigned)(counter.LowPart),
        (unsigned)(counter.HighPart ^ GetTickCount()));

    _snwprintf_s(szPipeIn, ARRAYSIZE(szPipeIn), _TRUNCATE, L"%s%s", szBase, CLI_PIPE_STDIN);
    _snwprintf_s(szPipeOut, ARRAYSIZE(szPipeOut), _TRUNCATE, L"%s%s", szBase, CLI_PIPE_STDOUT);
    _snwprintf_s(szPipeErr, ARRAYSIZE(szPipeErr), _TRUNCATE, L"%s%s", szBase, CLI_PIPE_STDERR);

    //
    // The broker talks to the pipes as a client, so all three instances are
    // created here, before the elevated copy exists.
    //
    hIn = CreateNamedPipeW(szPipeIn,
        PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, CLI_PIPE_BUF_SIZE, CLI_PIPE_BUF_SIZE, 0, NULL);

    hOut = CreateNamedPipeW(szPipeOut,
        PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, CLI_PIPE_BUF_SIZE, CLI_PIPE_BUF_SIZE, 0, NULL);

    hErr = CreateNamedPipeW(szPipeErr,
        PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, CLI_PIPE_BUF_SIZE, CLI_PIPE_BUF_SIZE, 0, NULL);

    if (hIn == INVALID_HANDLE_VALUE || hOut == INVALID_HANDLE_VALUE || hErr == INVALID_HANDLE_VALUE) {
        CliError(L"CreateNamedPipe failed, error %lu", GetLastError());
        goto Cleanup;
    }

    CliTrace(L"pipes ready: %s", szBase);

    //
    // Elevate a copy of this image as the broker, pipe base name only.
    //
    if (GetModuleFileNameW(NULL, szExe, ARRAYSIZE(szExe)) == 0) {
        CliError(L"GetModuleFileName failed, error %lu", GetLastError());
        goto Cleanup;
    }

    _snwprintf_s(szBrokerCmdLine, ARRAYSIZE(szBrokerCmdLine), _TRUNCATE,
        L"\"%s\" \"%s\"", szExe, szBase);

    CliTrace(L"elevating: %s", szBrokerCmdLine);

    if (CliNoElevateRequested()) {
        STARTUPINFOW si;
        RtlSecureZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        CliTrace(L"CLI_ELEVATOR_NO_ELEVATE set, starting the broker directly");
        if (!CreateProcessW(NULL, szBrokerCmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &payload)) {
            CliError(L"cannot start broker, error %lu", GetLastError());
            goto Cleanup;
        }
    }
    else if (!CliElevateAndLaunch(szBrokerCmdLine, &payload)) {
        CliError(L"elevation failed - UAC declined, not a member of Administrators, "
            L"or the bypass was blocked");
        goto Cleanup;
    }

    CliTrace(L"broker pid %u, waiting for it to connect", payload.dwProcessId);

    //
    // The broker landed in the job by inheritance. If this process could not
    // join its own job, assign the broker directly as the next best thing - the
    // target the broker starts inherits from the broker either way.
    //
    if (hKillJob) {
        BOOL inJob = FALSE;

        if (!joinedBySelf && !AssignProcessToJobObject(hKillJob, payload.hProcess))
            CliTrace(L"launcher: AssignProcessToJobObject(broker) failed, error %lu",
                GetLastError());

        if (IsProcessInJob(payload.hProcess, hKillJob, &inJob))
            CliTrace(L"launcher: broker in the kill job: %s", inJob ? L"yes" : L"no");
    }

    //
    // Wait for the broker to connect, bail out if it dies first.
    //
    hConnected = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (hConnected == NULL) {
        CliError(L"CreateEvent failed, error %lu", GetLastError());
        goto Cleanup;
    }

    connectCtx.Pipes[0] = hIn;
    connectCtx.Pipes[1] = hOut;
    connectCtx.Pipes[2] = hErr;
    connectCtx.DoneEvent = hConnected;

    {
        HANDLE hConnectThread = CreateThread(NULL, 0, CliConnectThread, &connectCtx, 0, NULL);
        HANDLE waiters[2];
        DWORD waitResult;

        if (hConnectThread == NULL) {
            CliError(L"CreateThread failed, error %lu", GetLastError());
            goto Cleanup;
        }

        waiters[0] = hConnected;
        waiters[1] = payload.hProcess;

        waitResult = WaitForMultipleObjects(2, waiters, FALSE, INFINITE);

        CloseHandle(hConnectThread);

        if (waitResult == WAIT_OBJECT_0 + 1) {
            GetExitCodeProcess(payload.hProcess, &exitCode);
            CliError(L"broker exited before connecting (code 0x%08X)", exitCode);
            exitCode = 1;
            goto Cleanup;
        }

        if (waitResult != WAIT_OBJECT_0 || connectCtx.Failed) {
            CliError(L"broker failed to connect, error %lu", GetLastError());
            goto Cleanup;
        }
    }

    //
    // Hand over the raw command line and this directory, which the broker only
    // needs to resolve a relative program path. Framing: DWORD command line
    // bytes, DWORD directory bytes, then both NUL terminated UTF-16 strings.
    //
    szCurrentDirectory[0] = 0;
    if (GetCurrentDirectoryW(ARRAYSIZE(szCurrentDirectory), szCurrentDirectory) == 0)
        szCurrentDirectory[0] = 0;

    cbCommandLine = (DWORD)((wcslen(RawCommandLine) + 1) * sizeof(WCHAR));
    cbCurrentDirectory = (DWORD)((wcslen(szCurrentDirectory) + 1) * sizeof(WCHAR));

    if (!CliWriteAll(hIn, &cbCommandLine, sizeof(cbCommandLine)) ||
        !CliWriteAll(hIn, &cbCurrentDirectory, sizeof(cbCurrentDirectory)) ||
        !CliWriteAll(hIn, RawCommandLine, cbCommandLine) ||
        !CliWriteAll(hIn, szCurrentDirectory, cbCurrentDirectory))
    {
        CliError(L"failed to send command line to broker, error %lu", GetLastError());
        goto Cleanup;
    }

    //
    // From here on Ctrl+C is forwarded to the broker.
    //
    g_hPipeIn = hIn;
    InterlockedExchange(&g_ForwardReady, 1);

    if (!SetConsoleCtrlHandler(CliCtrlHandler, TRUE)) {
        CliError(L"SetConsoleCtrlHandler failed, error %lu", GetLastError());
        goto Cleanup;
    }

    //
    // Target stdout/stderr back to this console, console input to the target.
    //
    outRelay.Source = hOut;
    outRelay.Sink = GetStdHandle(STD_OUTPUT_HANDLE);
    errRelay.Source = hErr;
    errRelay.Sink = GetStdHandle(STD_ERROR_HANDLE);

    hThreads[0] = CreateThread(NULL, 0, CliRelayThread, &outRelay, 0, NULL);
    hThreads[1] = CreateThread(NULL, 0, CliRelayThread, &errRelay, 0, NULL);
    hThreads[2] = CreateThread(NULL, 0, CliStdinThread, hIn, 0, NULL);

    if (hThreads[0] == NULL || hThreads[1] == NULL) {
        CliError(L"CreateThread failed, error %lu", GetLastError());
        goto Cleanup;
    }

    //
    // The launcher only exits after the broker does.
    //
    WaitForSingleObject(payload.hProcess, INFINITE);
    GetExitCodeProcess(payload.hProcess, &exitCode);
    result = (int)exitCode;

    //
    // Give the output relays a moment to drain what is still in the pipes.
    //
    if (hThreads[0] && hThreads[1])
        WaitForMultipleObjects(2, hThreads, TRUE, 10000);

Cleanup:
    g_ForwardReady = 0;

    //
    // The relays normally end by themselves once the broker is gone, this only
    // covers the paths that leave them parked in a blocking read. See
    // CliStopThread. The stdin thread reads the console, it is left alone.
    //
    CliStopThread(hThreads[0], 2000);
    CliStopThread(hThreads[1], 2000);

    if (hConnected)
        CloseHandle(hConnected);
    if (g_hPipeIn == hIn && g_hPipeIn != INVALID_HANDLE_VALUE)
        g_hPipeIn = NULL;
    if (hIn != INVALID_HANDLE_VALUE)
        CloseHandle(hIn);
    if (hOut != INVALID_HANDLE_VALUE)
        CloseHandle(hOut);
    if (hErr != INVALID_HANDLE_VALUE)
        CloseHandle(hErr);
    if (payload.hThread)
        CloseHandle(payload.hThread);
    if (payload.hProcess)
        CloseHandle(payload.hProcess);

    return result;
}

/*
* CliUsage
*
*/
static VOID CliUsage(
    VOID)
{
    fwprintf(stderr,
        L"cli-elevator - run a program elevated, with its stdio piped back here\n"
        L"\n"
        L"Usage:\n"
        L"  cli-elevator <command line>\n"
        L"      Create the pipes, elevate a copy of this image (broker), send\n"
        L"      <command line> to it and relay stdio until it exits.\n"
        L"\n"
        L"  cli-elevator \\\\.\\pipe\\<base>\n"
        L"      Broker mode, internal. Connects to <base>-stdin, <base>-stdout\n"
        L"      and <base>-stderr and runs the command line it receives.\n"
        L"\n"
        L"Examples:\n"
        L"  cli-elevator cmd.exe /c whoami\n"
        L"  cli-elevator \"C:\\Program Files\\Git\\bin\\bash.exe\" -i\n");
    fflush(stderr);
}

/*
* wmain
*
* Purpose:
*
* Select the mode and run it.
*
*/
int wmain(
    int argc,
    wchar_t **argv)
{
    LPCWSTR raw;
    WCHAR szToken[MAX_PATH];
    BOOL isPipe;

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    raw = CliSkipProgramName(GetCommandLineW());

    if (*raw == 0) {
        CliUsage();
        return 1;
    }

    if (!CliFirstToken(raw, szToken, ARRAYSIZE(szToken))) {
        CliUsage();
        return 1;
    }

    if (_wcsicmp(szToken, L"-h") == 0 ||
        _wcsicmp(szToken, L"--help") == 0 ||
        _wcsicmp(szToken, L"/?") == 0)
    {
        CliUsage();
        return 0;
    }

    isPipe = CliIsPipeName(szToken);

    if (isPipe) {
        //
        // Broker mode. Hide the console first thing, never close it: the
        // target process shares it and control events are delivered through it.
        //
        HWND hwndConsole = GetConsoleWindow();
        if (hwndConsole)
            ShowWindow(hwndConsole, SW_HIDE);

        return CliRunBroker(szToken);
    }

    return CliRunLauncher(raw);
}
