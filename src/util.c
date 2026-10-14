/*******************************************************************************
*
*  cli-elevator - small helpers shared by the launcher and the broker.
*
*******************************************************************************/

#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#include "cli-elevator.h"

/*
* CliReadAll
*
* Purpose:
*
* Read exactly Length bytes. FALSE on EOF or error.
*
*/
BOOL CliReadAll(
    HANDLE Handle,
    VOID *Buffer,
    DWORD Length)
{
    BYTE *p = (BYTE *)Buffer;
    DWORD read = 0;

    while (Length) {
        if (!ReadFile(Handle, p, Length, &read, NULL) || read == 0)
            return FALSE;
        p += read;
        Length -= read;
    }

    return TRUE;
}

/*
* CliWriteAll
*
* Purpose:
*
* Write the whole buffer, loop on partial writes. FALSE on error.
*
*/
BOOL CliWriteAll(
    HANDLE Handle,
    const VOID *Buffer,
    DWORD Length)
{
    const BYTE *p = (const BYTE *)Buffer;
    DWORD written = 0;

    while (Length) {
        if (!WriteFile(Handle, p, Length, &written, NULL) || written == 0)
            return FALSE;
        p += written;
        Length -= written;
    }

    return TRUE;
}

/*
* CliHandleUsable
*
*/
BOOL CliHandleUsable(
    HANDLE Handle)
{
    return (Handle != NULL) && (Handle != INVALID_HANDLE_VALUE);
}

/*
* CliSkipProgramName
*
* Purpose:
*
* Return CommandLine with the first (program name) token removed, i.e. exactly
* what the user typed behind "cli-elevator". Uses the CommandLineToArgvW rules
* for that first token.
*
*/
LPCWSTR CliSkipProgramName(
    LPCWSTR CommandLine)
{
    LPCWSTR p = CommandLine;

    while (*p == L' ' || *p == L'\t')
        p++;

    if (*p == L'"') {
        p++;
        while (*p && *p != L'"')
            p++;
        if (*p == L'"')
            p++;
    }
    else {
        while (*p && *p != L' ' && *p != L'\t')
            p++;
    }

    while (*p == L' ' || *p == L'\t')
        p++;

    return p;
}

/*
* CliFirstToken
*
* Purpose:
*
* Copy the first token of CommandLine into Buffer, quotes are dropped, single
* quotes are accepted as a convenience for shell habits.
*
*/
BOOL CliFirstToken(
    LPCWSTR CommandLine,
    LPWSTR Buffer,
    SIZE_T cchBuffer)
{
    LPCWSTR p = CommandLine;
    SIZE_T i = 0;
    WCHAR quote = 0;

    while (*p == L' ' || *p == L'\t')
        p++;

    if (*p == L'"' || *p == L'\'') {
        quote = *p;
        p++;
    }

    while (*p) {
        if (quote ? (*p == quote) : (*p == L' ' || *p == L'\t'))
            break;
        if (i + 1 >= cchBuffer)
            return FALSE;
        Buffer[i++] = *p++;
    }

    Buffer[i] = 0;
    return (i > 0);
}

/*
* CliTrace
*
* Purpose:
*
* Progress reporting, off unless CLI_ELEVATOR_TRACE is set in the environment.
* The broker's console is hidden with nothing listening on it, so when
* CLI_ELEVATOR_TRACE_FILE names a file the lines go there instead.
*
*/
VOID CliTrace(
    LPCWSTR Format,
    ...)
{
    static volatile LONG State = -1;
    static HANDLE LogFile = INVALID_HANDLE_VALUE;
    WCHAR szBuffer[1024];
    WCHAR szPath[MAX_PATH];
    WCHAR szValue[8];
    WCHAR szLine[1100];
    va_list args;
    DWORD written = 0;

    if (InterlockedCompareExchange(&State, 0, -1) == -1) {
        LONG enabled = 0;
        if (GetEnvironmentVariableW(L"CLI_ELEVATOR_TRACE", szValue, ARRAYSIZE(szValue)) > 0)
            enabled = 1;
        InterlockedExchange(&State, enabled);
    }

    if (InterlockedCompareExchange(&State, 0, 0) == 0)
        return;

    va_start(args, Format);
    _vsnwprintf_s(szBuffer, ARRAYSIZE(szBuffer), _TRUNCATE, Format, args);
    va_end(args);

    if (InterlockedCompareExchangePointer((PVOID *)&LogFile, NULL, (PVOID)INVALID_HANDLE_VALUE)
        == (PVOID)INVALID_HANDLE_VALUE)
    {
        HANDLE h = INVALID_HANDLE_VALUE;

        if (GetEnvironmentVariableW(L"CLI_ELEVATOR_TRACE_FILE", szPath, ARRAYSIZE(szPath)) > 0)
            h = CreateFileW(szPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

        InterlockedExchangePointer((PVOID *)&LogFile, h);
    }

    _snwprintf_s(szLine, ARRAYSIZE(szLine), _TRUNCATE, L"[trace] %u: %s\r\n",
        (unsigned)GetCurrentProcessId(), szBuffer);

    if (LogFile != NULL && LogFile != INVALID_HANDLE_VALUE)
        WriteFile(LogFile, szLine, (DWORD)(wcslen(szLine) * sizeof(WCHAR)), &written, NULL);

    fwprintf(stderr, L"%s", szLine);
    fflush(stderr);
}

/*
* CliStopThread
*
* Purpose:
*
* Ask a thread that is parked in a blocking read to give up and wait for it.
*
* This exists because CloseHandle on a handle that another thread is blocked
* reading waits in the kernel for that read to finish, and reads on our pipes can
* legitimately never finish - the peer keeps its end open on purpose. Cancel the
* read first, then the handle can be closed.
*
*/
BOOL CliStopThread(
    HANDLE hThread,
    DWORD TimeoutMs)
{
    ULONGLONG start;

    if (hThread == NULL)
        return TRUE;

    start = GetTickCount64();

    for (;;) {
        CancelSynchronousIo(hThread);

        if (WaitForSingleObject(hThread, 20) == WAIT_OBJECT_0)
            return TRUE;

        if (GetTickCount64() - start >= TimeoutMs)
            return FALSE;
    }
}

/*
* CliIsPathRooted
*
* Purpose:
*
* TRUE for "C:\x" and "\x" and "\\server\share\x" style paths, FALSE for
* "x", ".\x", "..\x".
*
*/
BOOL CliIsPathRooted(
    LPCWSTR Path)
{
    if (Path[0] == L'\\' || Path[0] == L'/')
        return TRUE;
    if (Path[0] != 0 && Path[1] == L':')
        return TRUE;
    return FALSE;
}

/*
* CliCopyString
*
* Purpose:
*
* Bounded string copy, FALSE if the result had to be truncated. Unlike the
* Annex K wcscpy_s it never trips the invalid parameter handler.
*
*/
BOOL CliCopyString(
    LPWSTR Buffer,
    SIZE_T cchBuffer,
    LPCWSTR Source)
{
    return (_snwprintf_s(Buffer, cchBuffer, _TRUNCATE, L"%s", Source) >= 0);
}

/*
* CliCombinePath
*
* Purpose:
*
* Base + separator (when missing) + Name.
*
*/
BOOL CliCombinePath(
    LPCWSTR Base,
    LPCWSTR Name,
    LPWSTR Buffer,
    SIZE_T cchBuffer)
{
    size_t len;

    if (Base == NULL || Base[0] == 0)
        return CliCopyString(Buffer, cchBuffer, Name);

    len = wcslen(Base);

    if (Base[len - 1] == L'\\' || Base[len - 1] == L'/')
        return (_snwprintf_s(Buffer, cchBuffer, _TRUNCATE, L"%s%s", Base, Name) >= 0);

    return (_snwprintf_s(Buffer, cchBuffer, _TRUNCATE, L"%s\\%s", Base, Name) >= 0);
}
