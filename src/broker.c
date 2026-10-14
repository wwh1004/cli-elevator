/*******************************************************************************
*
*  cli-elevator - broker (mode b).
*
*  Runs elevated, started by the launcher with a single argument:
*
*      cli-elevator \\.\pipe\<base>
*
*  What it does, in order:
*
*    1. hides its own console - hides, not closes it, the target process shares
*       that console and control events are delivered through it.
*    2. connects to <base>-stdin, <base>-stdout and <base>-stderr.
*    3. reads the raw command line the launcher typed, plus the launcher's
*       current directory (only used to resolve a relative program path).
*    4. resolves the program, takes the program's own directory as the working
*       directory, creates anonymous pipes and starts the target with its stdio
*       bound to them.
*    5. relays target stdout/stderr upstream and launcher stdin down to the
*       target. A 0x03 byte on the stdin pipe means the user pressed Ctrl+C,
*       it is turned into GenerateConsoleCtrlEvent for the target process group.
*    6. exits with the target exit code.
*
*******************************************************************************/

#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#include "cli-elevator.h"

#define CLI_CONNECT_TIMEOUT_MS  10000

#pragma comment(lib, "user32.lib")

typedef struct _RELAY_CTX {
    HANDLE Source;
    HANDLE Sink;
} RELAY_CTX;

typedef struct _STDIN_CTX {
    HANDLE PipeIn;      /* launcher -> broker */
    HANDLE ChildStdin;  /* broker -> target */
    HANDLE ChildProcess;
    DWORD  ChildPid;
} STDIN_CTX;

static VOID CliBrokerError(
    LPCWSTR Format,
    ...);

/*
* CliTargetSharesConsole
*
* Purpose:
*
* TRUE when the target showed up in this process console process list, i.e. it
* lives on the same console and control events aimed at that console reach it.
*
*/
static BOOL CliTargetSharesConsole(
    DWORD ChildPid)
{
    DWORD pids[64];
    DWORD count, i;
    WCHAR szList[400];
    WCHAR szItem[24];
    HANDLE hConIn;

    count = GetConsoleProcessList(pids, ARRAYSIZE(pids));
    if (count == 0) {
        CliTrace(L"broker: no console attached, error %lu", GetLastError());
        return FALSE;
    }

    if (count > ARRAYSIZE(pids))
        count = ARRAYSIZE(pids);

    szList[0] = 0;
    for (i = 0; i < count; i++) {
        _snwprintf_s(szItem, ARRAYSIZE(szItem), _TRUNCATE, L" %lu", pids[i]);
        wcscat_s(szList, ARRAYSIZE(szList), szItem);
    }

    hConIn = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, 0, NULL);

    CliTrace(L"broker: self %lu, window %p, CONIN$ %s, console procs:%s",
        (unsigned)GetCurrentProcessId(), GetConsoleWindow(),
        (hConIn == INVALID_HANDLE_VALUE) ? L"none" : L"ok", szList);

    if (hConIn != INVALID_HANDLE_VALUE)
        CloseHandle(hConIn);

    for (i = 0; i < count; i++) {
        if (pids[i] == ChildPid)
            return TRUE;
    }

    return FALSE;
}

/*
* CliInjectCtrlC
*
* Purpose:
*
* Push a Ctrl+C key record into the console input buffer this process is
* attached to - byte for byte what a user pressing Ctrl+C puts there. Falls out
* of the console event plumbing that GenerateConsoleCtrlEvent depends on, which
* some console hosts get wrong.
*
*/
static BOOL CliInjectCtrlC(
    VOID)
{
    HANDLE hConIn;
    INPUT_RECORD ir[2];
    DWORD written = 0;
    BOOL bResult = FALSE;

    hConIn = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

    if (hConIn == INVALID_HANDLE_VALUE) {
        CliTrace(L"broker: CONIN$ unavailable, error %lu", GetLastError());
        return FALSE;
    }

    RtlSecureZeroMemory(ir, sizeof(ir));

    ir[0].EventType = KEY_EVENT;
    ir[0].Event.KeyEvent.bKeyDown = TRUE;
    ir[0].Event.KeyEvent.wRepeatCount = 1;
    ir[0].Event.KeyEvent.wVirtualKeyCode = (WORD)'C';
    ir[0].Event.KeyEvent.wVirtualScanCode = (WORD)MapVirtualKeyW('C', MAPVK_VK_TO_VSC);
    ir[0].Event.KeyEvent.uChar.UnicodeChar = CLI_CTRL_C_BYTE;
    ir[0].Event.KeyEvent.dwControlKeyState = LEFT_CTRL_PRESSED;

    ir[1] = ir[0];
    ir[1].Event.KeyEvent.bKeyDown = FALSE;
    ir[1].Event.KeyEvent.uChar.UnicodeChar = 0;
    ir[1].Event.KeyEvent.dwControlKeyState = 0;

    if (WriteConsoleInputW(hConIn, ir, ARRAYSIZE(ir), &written))
        bResult = TRUE;
    else
        CliTrace(L"broker: WriteConsoleInput failed, error %lu", GetLastError());

    CloseHandle(hConIn);
    return bResult;
}

/*
* CliSignalConsole
*
* Purpose:
*
* Get the Ctrl+C across on the console this process is attached to. The target
* and this process share that console, see CliSendCtrlToTarget.
*
* The documented call goes first, the key record follows unconditionally: a
* console host that drops one of them still gets the other, and a second Ctrl+C
* arriving at a process that already died on the first one costs nothing.
*
* Note both go to the whole console (group id 0, which is the only form
* CTRL_C_EVENT supports) - the target, anything the target started, and this
* process. That is why CliBrokerCtrlHandler swallows its own copy.
*
*/
static VOID CliSignalConsole(
    VOID)
{
    BOOL sentEvent = GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);

    if (sentEvent)
        CliTrace(L"broker: ctrl-c sent as CTRL_C_EVENT for the console");
    else
        CliTrace(L"broker: CTRL_C_EVENT failed, error %lu", GetLastError());

    if (CliInjectCtrlC())
        CliTrace(L"broker: ctrl-c sent as a key record");

    //
    // Only if the API itself refused: Ctrl+Break is a different signal and a
    // program that meant to handle Ctrl+C would be killed by it.
    //
    if (!sentEvent) {
        if (GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0))
            CliTrace(L"broker: ctrl-c sent as CTRL_BREAK_EVENT for the console");
    }
}

/*
* CliSendCtrlToTarget
*
* Purpose:
*
* Hand the Ctrl+C the user pressed to the target. Normally the target sits on
* this console and CliSignalConsole is enough; if it somehow ended up on a
* console of its own, attach to that one first.
*
*/
static VOID CliSendCtrlToTarget(
    STDIN_CTX *ctx)
{
    //
    // Ask now, not when the target was started: a freshly created process is not
    // in the console process list yet, and it is the list at Ctrl+C time that
    // decides which route below is taken.
    //
    BOOL sharesConsole = CliTargetSharesConsole(ctx->ChildPid);

    if (sharesConsole) {
        CliSignalConsole();
    }
    else {
        CliTrace(L"broker: target is not on our console, attaching to %u", (unsigned)ctx->ChildPid);

        FreeConsole();
        if (AttachConsole(ctx->ChildPid)) {
            CliSignalConsole();
            FreeConsole();
        }
        else {
            CliBrokerError(L"AttachConsole(%lu) failed, error %lu",
                (unsigned)ctx->ChildPid, GetLastError());
        }
    }

    //
    // Informational only: a console program is free to ignore Ctrl+C, so this
    // just says whether the target noticed.
    //
    if (WaitForSingleObject(ctx->ChildProcess, 300) == WAIT_OBJECT_0)
        CliTrace(L"broker: target exited after the Ctrl+C");
    else
        CliTrace(L"broker: target still running after the Ctrl+C");
}

/*
* CliBrokerError
*
* Purpose:
*
* Report a broker side failure. The console is hidden, so this is only visible
* when the broker is started by hand from a console.
*
*/
static VOID CliBrokerError(
    LPCWSTR Format,
    ...)
{
    WCHAR szBuffer[1024];
    va_list args;

    va_start(args, Format);
    _vsnwprintf_s(szBuffer, ARRAYSIZE(szBuffer), _TRUNCATE, Format, args);
    va_end(args);

    CliTrace(L"broker error: %s", szBuffer);

    fwprintf(stderr, L"cli-elevator(broker): %s\n", szBuffer);
    fflush(stderr);
}

/*
* CliBrokerConnectPipe
*
* Purpose:
*
* Connect to one of the launcher created pipe instances, retrying while the
* server side has not finished creating them.
*
*/
static HANDLE CliBrokerConnectPipe(
    LPCWSTR PipeName,
    DWORD DesiredAccess)
{
    ULONGLONG start = GetTickCount64();

    for (;;) {
        HANDLE h = CreateFileW(PipeName, DesiredAccess, 0, NULL,
            OPEN_EXISTING, 0, NULL);

        if (h != INVALID_HANDLE_VALUE)
            return h;

        if (GetTickCount64() - start >= CLI_CONNECT_TIMEOUT_MS)
            return INVALID_HANDLE_VALUE;

        Sleep(20);
    }
}

/*
* CliBrokerSurviveSignals
*
* Purpose:
*
* The target process group gets the control events, the broker must not react
* to the ones the console broadcasts to everyone.
*
*/
static BOOL WINAPI CliBrokerCtrlHandler(
    DWORD CtrlType)
{
    UNREFERENCED_PARAMETER(CtrlType);
    return TRUE;
}

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
* CliStdinRelayThread
*
* Purpose:
*
* Launcher stdin -> target stdin. A lone 0x03 is not data, it is the user
* pressing Ctrl+C, and it becomes a control event for the target process group.
*
*/
static DWORD WINAPI CliStdinRelayThread(
    LPVOID Parameter)
{
    STDIN_CTX *ctx = (STDIN_CTX *)Parameter;
    BYTE buffer[4096];
    DWORD read = 0;
    DWORD i, start;

    for (;;) {

        if (!ReadFile(ctx->PipeIn, buffer, sizeof(buffer), &read, NULL)) {
            CliTrace(L"broker: upstream stdin pipe broken, forwarding stops (error %lu)",
                GetLastError());
            break;
        }

        if (read == 0) {
            CliTrace(L"broker: upstream stdin pipe closed, forwarding stops");
            break;
        }

        //
        // Walk the chunk, handing the 0x03 markers to the console and
        // everything between them to the target in one write.
        //
        start = 0;
        for (i = 0; i < read; i++) {

            if (buffer[i] != CLI_CTRL_C_BYTE)
                continue;

            if (i > start && !CliWriteAll(ctx->ChildStdin, buffer + start, i - start)) {
                CliTrace(L"broker: target stdin gone, forwarding stops");
                return 0;
            }

            CliTrace(L"broker: ctrl-c byte from the launcher");
            CliSendCtrlToTarget(ctx);

            start = i + 1;
        }

        if (read > start && !CliWriteAll(ctx->ChildStdin, buffer + start, read - start)) {
            CliTrace(L"broker: target stdin gone, forwarding stops");
            return 0;
        }
    }

    return 0;
}

/*
* CliIsPathless
*
* Purpose:
*
* TRUE if the name carries no directory part, so it has to go through the
* search path instead of straight to the file system.
*
*/
static BOOL CliIsPathless(
    LPCWSTR Name)
{
    return (wcschr(Name, L'\\') == NULL) && (wcschr(Name, L'/') == NULL);
}

/*
* CliResolveTarget
*
* Purpose:
*
* Turn the first token of the command line into an image path and the working
* directory that goes with it - the directory the program itself lives in.
*
*/
static BOOL CliResolveTarget(
    LPCWSTR AppName,
    LPCWSTR LauncherDirectory,
    LPWSTR Image,
    SIZE_T cchImage,
    LPWSTR WorkingDirectory,
    SIZE_T cchWorkingDirectory)
{
    WCHAR szCombined[MAX_PATH * 4];
    WCHAR szFull[MAX_PATH * 4];
    LPWSTR p;

    szFull[0] = 0;

    if (CliIsPathless(AppName)) {

        //
        // Bare program name, same lookup CreateProcess would do, so that
        // "cli-elevator ping 127.0.0.1" works.
        //
        if (SearchPathW(NULL, AppName, L".exe", ARRAYSIZE(szFull), szFull, NULL) == 0) {

            //
            // Not on the search path either, fall back to the launcher
            // directory before giving up.
            //
            if (!CliCombinePath(LauncherDirectory, AppName, szCombined, ARRAYSIZE(szCombined)))
                return FALSE;

            if (GetFileAttributesW(szCombined) == INVALID_FILE_ATTRIBUTES)
                return FALSE;

            if (GetFullPathNameW(szCombined, ARRAYSIZE(szFull), szFull, NULL) == 0)
                return FALSE;
        }
    }
    else {

        //
        // A path. A relative one is relative to the launcher, this process sits
        // in the system directory and its idea of "." is useless to the user.
        //
        if (!CliIsPathRooted(AppName)) {
            if (!CliCombinePath(LauncherDirectory, AppName, szCombined, ARRAYSIZE(szCombined)))
                return FALSE;
        }
        else {
            if (!CliCopyString(szCombined, ARRAYSIZE(szCombined), AppName))
                return FALSE;
        }

        if (GetFullPathNameW(szCombined, ARRAYSIZE(szFull), szFull, NULL) == 0)
            return FALSE;
    }

    if (!CliCopyString(Image, cchImage, szFull))
        return FALSE;

    //
    // Working directory = directory of the program.
    //
    if (!CliCopyString(WorkingDirectory, cchWorkingDirectory, szFull))
        return FALSE;

    p = wcsrchr(WorkingDirectory, L'\\');
    if (p == NULL)
        return FALSE;

    if (p == WorkingDirectory + 2)
        p[1] = 0;           /* "C:\" keeps its backslash */
    else
        *p = 0;

    return TRUE;
}

/*
* CliRunBroker
*
* Purpose:
*
* Mode b, see file header.
*
*/
int CliRunBroker(
    LPCWSTR PipeBaseName)
{
    WCHAR szName[MAX_PATH];
    WCHAR szAppName[MAX_PATH * 2];
    WCHAR szImage[MAX_PATH * 2];
    WCHAR szWorkingDirectory[MAX_PATH * 2];
    HANDLE hPipeIn = INVALID_HANDLE_VALUE;
    HANDLE hPipeOut = INVALID_HANDLE_VALUE;
    HANDLE hPipeErr = INVALID_HANDLE_VALUE;
    HANDLE hChildStdin = INVALID_HANDLE_VALUE;
    HANDLE hChildStdout = INVALID_HANDLE_VALUE;
    HANDLE hChildStderr = INVALID_HANDLE_VALUE;
    HANDLE hChildStdinRead = INVALID_HANDLE_VALUE;
    HANDLE hChildStdoutWrite = INVALID_HANDLE_VALUE;
    HANDLE hChildStderrWrite = INVALID_HANDLE_VALUE;
    HANDLE hRelayOut = NULL, hRelayErr = NULL, hRelayIn = NULL;
    PWSTR pszCommandLine = NULL;
    PWSTR pszLauncherDirectory = NULL;
    DWORD cbCommandLine = 0, cbLauncherDirectory = 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    RELAY_CTX outRelay, errRelay;
    STDIN_CTX stdinRelay;
    DWORD exitCode = 1;
    DWORD waitResult;
    int result = 1;

    RtlSecureZeroMemory(&si, sizeof(si));
    RtlSecureZeroMemory(&pi, sizeof(pi));
    RtlSecureZeroMemory(&outRelay, sizeof(outRelay));
    RtlSecureZeroMemory(&errRelay, sizeof(errRelay));
    RtlSecureZeroMemory(&stdinRelay, sizeof(stdinRelay));

    SetConsoleCtrlHandler(CliBrokerCtrlHandler, TRUE);

    CliTrace(L"broker entered, base %s", PipeBaseName);

    //
    // Connect upstream first: if the launcher is gone there is nothing to do.
    //
    _snwprintf_s(szName, ARRAYSIZE(szName), _TRUNCATE, L"%s%s", PipeBaseName, CLI_PIPE_STDIN);
    hPipeIn = CliBrokerConnectPipe(szName, GENERIC_READ);
    if (hPipeIn == INVALID_HANDLE_VALUE) {
        CliBrokerError(L"cannot connect to %s, error %lu", szName, GetLastError());
        goto Cleanup;
    }

    _snwprintf_s(szName, ARRAYSIZE(szName), _TRUNCATE, L"%s%s", PipeBaseName, CLI_PIPE_STDOUT);
    hPipeOut = CliBrokerConnectPipe(szName, GENERIC_WRITE);
    if (hPipeOut == INVALID_HANDLE_VALUE) {
        CliBrokerError(L"cannot connect to %s, error %lu", szName, GetLastError());
        goto Cleanup;
    }

    _snwprintf_s(szName, ARRAYSIZE(szName), _TRUNCATE, L"%s%s", PipeBaseName, CLI_PIPE_STDERR);
    hPipeErr = CliBrokerConnectPipe(szName, GENERIC_WRITE);
    if (hPipeErr == INVALID_HANDLE_VALUE) {
        CliBrokerError(L"cannot connect to %s, error %lu", szName, GetLastError());
        goto Cleanup;
    }

    CliTrace(L"broker connected to all three pipes");

    //
    // Framed hand over: DWORD command line bytes, DWORD launcher directory
    // bytes, then both strings, NUL terminated UTF-16.
    //
    if (!CliReadAll(hPipeIn, &cbCommandLine, sizeof(cbCommandLine)) ||
        !CliReadAll(hPipeIn, &cbLauncherDirectory, sizeof(cbLauncherDirectory)))
    {
        CliBrokerError(L"launcher did not send a command line");
        goto Cleanup;
    }

    if (cbCommandLine < sizeof(WCHAR) || cbCommandLine > CLI_MAX_CMDLINE ||
        (cbCommandLine & 1) != 0 ||
        cbLauncherDirectory > CLI_MAX_CMDLINE || (cbLauncherDirectory & 1) != 0)
    {
        CliBrokerError(L"bad command line framing (%lu/%lu bytes)",
            cbCommandLine, cbLauncherDirectory);
        goto Cleanup;
    }

    pszCommandLine = (PWSTR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        cbCommandLine + sizeof(WCHAR));
    if (pszCommandLine == NULL) {
        CliBrokerError(L"out of memory");
        goto Cleanup;
    }

    if (!CliReadAll(hPipeIn, pszCommandLine, cbCommandLine)) {
        CliBrokerError(L"short read on the command line");
        goto Cleanup;
    }
    pszCommandLine[cbCommandLine / sizeof(WCHAR)] = 0;

    if (cbLauncherDirectory) {
        pszLauncherDirectory = (PWSTR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            cbLauncherDirectory + sizeof(WCHAR));
        if (pszLauncherDirectory == NULL) {
            CliBrokerError(L"out of memory");
            goto Cleanup;
        }
        if (!CliReadAll(hPipeIn, pszLauncherDirectory, cbLauncherDirectory)) {
            CliBrokerError(L"short read on the launcher directory");
            goto Cleanup;
        }
        pszLauncherDirectory[cbLauncherDirectory / sizeof(WCHAR)] = 0;
    }
    else {
        pszLauncherDirectory = (PWSTR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WCHAR));
        if (pszLauncherDirectory == NULL)
            goto Cleanup;
    }

    if (!CliFirstToken(pszCommandLine, szAppName, ARRAYSIZE(szAppName))) {
        CliBrokerError(L"cannot find a program name in \"%s\"", pszCommandLine);
        goto Cleanup;
    }

    CliTrace(L"broker got command line \"%s\" from \"%s\"", pszCommandLine, pszLauncherDirectory);

    if (!CliResolveTarget(szAppName, pszLauncherDirectory,
        szImage, ARRAYSIZE(szImage),
        szWorkingDirectory, ARRAYSIZE(szWorkingDirectory)))
    {
        CliBrokerError(L"cannot resolve \"%s\", error %lu", szAppName, GetLastError());
        goto Cleanup;
    }

    CliTrace(L"broker resolved image \"%s\", working dir \"%s\"", szImage, szWorkingDirectory);

    //
    // Anonymous pipes for the target stdio, child ends inheritable only.
    //
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&hChildStdinRead, &hChildStdin, &sa, 0) ||
        !CreatePipe(&hChildStdout, &hChildStdoutWrite, &sa, 0) ||
        !CreatePipe(&hChildStderr, &hChildStderrWrite, &sa, 0))
    {
        CliBrokerError(L"CreatePipe failed, error %lu", GetLastError());
        goto Cleanup;
    }

    SetHandleInformation(hChildStdin, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hChildStdout, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hChildStderr, HANDLE_FLAG_INHERIT, 0);

    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hChildStdinRead;
    si.hStdOutput = hChildStdoutWrite;
    si.hStdError = hChildStderrWrite;

    //
    // No CREATE_NEW_CONSOLE: the target shares the hidden console of this
    // process, which is what makes control events reach it. No
    // CREATE_NEW_PROCESS_GROUP either: that flag would switch Ctrl+C off inside
    // the target, and we want to hand it a plain Ctrl+C.
    //
    if (!CreateProcessW(szImage,
        pszCommandLine,
        NULL,
        NULL,
        TRUE,
        CREATE_UNICODE_ENVIRONMENT,
        NULL,
        szWorkingDirectory,
        &si,
        &pi))
    {
        CliBrokerError(L"cannot start \"%s\" in \"%s\", error %lu",
            szImage, szWorkingDirectory, GetLastError());
        goto Cleanup;
    }

    CloseHandle(hChildStdinRead);
    hChildStdinRead = INVALID_HANDLE_VALUE;
    CloseHandle(hChildStdoutWrite);
    hChildStdoutWrite = INVALID_HANDLE_VALUE;
    CloseHandle(hChildStderrWrite);
    hChildStderrWrite = INVALID_HANDLE_VALUE;

    CliTrace(L"broker started target pid %u", (unsigned)pi.dwProcessId);

    //
    // Relays: target stdout/stderr upstream, launcher stdin down to the target.
    //
    outRelay.Source = hChildStdout;
    outRelay.Sink = hPipeOut;
    errRelay.Source = hChildStderr;
    errRelay.Sink = hPipeErr;

    stdinRelay.PipeIn = hPipeIn;
    stdinRelay.ChildStdin = hChildStdin;
    stdinRelay.ChildPid = pi.dwProcessId;
    stdinRelay.ChildProcess = pi.hProcess;

    hRelayOut = CreateThread(NULL, 0, CliRelayThread, &outRelay, 0, NULL);
    hRelayErr = CreateThread(NULL, 0, CliRelayThread, &errRelay, 0, NULL);
    hRelayIn = CreateThread(NULL, 0, CliStdinRelayThread, &stdinRelay, 0, NULL);

    if (hRelayOut == NULL || hRelayErr == NULL || hRelayIn == NULL) {
        CliBrokerError(L"CreateThread failed, error %lu", GetLastError());
        TerminateProcess(pi.hProcess, 1);
    }

    //
    // Sit on the target. It is the target that decides how long we live.
    //
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    result = (int)exitCode;

    CliTrace(L"broker: target exited with %u", (unsigned)exitCode);

    //
    // Drain whatever the target left in the anon pipes before reporting back.
    //
    if (hRelayOut && hRelayErr) {
        HANDLE hRelays[2];
        hRelays[0] = hRelayOut;
        hRelays[1] = hRelayErr;
        waitResult = WaitForMultipleObjects(2, hRelays, TRUE, 30000);
        CliTrace(L"broker: relays drained (%u)", (unsigned)waitResult);
    }

    CliTrace(L"broker: leaving with %u", (unsigned)exitCode);

Cleanup:
    CliTrace(L"broker: cleanup start");

    //
    // Every relay thread sits in a blocking read that can legitimately never
    // finish, and closing a handle with such a read pending blocks in the
    // kernel. Cancel them first, see CliStopThread.
    //
    CliStopThread(hRelayOut, 2000);
    CliStopThread(hRelayErr, 2000);
    CliStopThread(hRelayIn, 5000);

    if (hChildStdinRead != INVALID_HANDLE_VALUE)
        CloseHandle(hChildStdinRead);
    if (hChildStdoutWrite != INVALID_HANDLE_VALUE)
        CloseHandle(hChildStdoutWrite);
    if (hChildStderrWrite != INVALID_HANDLE_VALUE)
        CloseHandle(hChildStderrWrite);
    if (hChildStdin != INVALID_HANDLE_VALUE)
        CloseHandle(hChildStdin);
    if (hChildStdout != INVALID_HANDLE_VALUE)
        CloseHandle(hChildStdout);
    if (hChildStderr != INVALID_HANDLE_VALUE)
        CloseHandle(hChildStderr);
    CliTrace(L"broker: anon pipes closed");
    if (hPipeIn != INVALID_HANDLE_VALUE)
        CloseHandle(hPipeIn);
    if (hPipeOut != INVALID_HANDLE_VALUE)
        CloseHandle(hPipeOut);
    if (hPipeErr != INVALID_HANDLE_VALUE)
        CloseHandle(hPipeErr);
    CliTrace(L"broker: upstream pipes closed");
    if (pi.hThread)
        CloseHandle(pi.hThread);
    if (pi.hProcess)
        CloseHandle(pi.hProcess);
    if (pszCommandLine)
        HeapFree(GetProcessHeap(), 0, pszCommandLine);
    if (pszLauncherDirectory)
        HeapFree(GetProcessHeap(), 0, pszLauncherDirectory);

    CliTrace(L"broker: cleanup done");
    return result;
}
