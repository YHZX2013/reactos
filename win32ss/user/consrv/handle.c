/*
 * LICENSE:         GPL - See COPYING in the top level directory
 * PROJECT:         ReactOS Console Server DLL
 * FILE:            win32ss/user/consrv/handle.c
 * PURPOSE:         Console I/O Handles functions
 * PROGRAMMERS:
 */

/* INCLUDES ******************************************************************/

#include "consrv.h"
#include "conio.h"

//#define NDEBUG
#include <debug.h>


/* PRIVATE FUNCTIONS *********************************************************/

static INT
AdjustHandleCounts(PCONSOLE_IO_HANDLE Entry, INT Change)
{
    Object_t *Object = Entry->Object;

    DPRINT1("AdjustHandleCounts(0x%p, %d), Object = 0x%p, Object->HandleCount = %d, Object->Type = %lu\n", Entry, Change, Object, Object->HandleCount, Object->Type);

    if (Entry->Access & GENERIC_READ)           Object->AccessRead += Change;
    if (Entry->Access & GENERIC_WRITE)          Object->AccessWrite += Change;
    if (!(Entry->ShareMode & FILE_SHARE_READ))  Object->ExclusiveRead += Change;
    if (!(Entry->ShareMode & FILE_SHARE_WRITE)) Object->ExclusiveWrite += Change;

    Object->HandleCount += Change;

    return Object->HandleCount;
}

static VOID
Win32CsrCreateHandleEntry(PCONSOLE_IO_HANDLE Entry)
{
    /// LOCK /// Object_t *Object = Entry->Object;
    /// LOCK /// EnterCriticalSection(&Object->Console->Lock);
    AdjustHandleCounts(Entry, +1);
    /// LOCK /// LeaveCriticalSection(&Object->Console->Lock);
}

static VOID
Win32CsrCloseHandleEntry(PCONSOLE_IO_HANDLE Entry)
{
    Object_t *Object = Entry->Object;
    if (Object != NULL)
    {
        /// LOCK /// PCONSOLE Console = Object->Console;
        /// LOCK /// EnterCriticalSection(&Console->Lock);

        /// TODO: HERE, trigger input waiting threads.

        /* If the last handle to a screen buffer is closed, delete it... */
        if (AdjustHandleCounts(Entry, -1) == 0)
        {
            if (Object->Type == CONIO_SCREEN_BUFFER_MAGIC)
            {
                PCONSOLE_SCREEN_BUFFER Buffer = (PCONSOLE_SCREEN_BUFFER)Object;
                /* ...unless it's the only buffer left. Windows allows deletion
                 * even of the last buffer, but having to deal with a lack of
                 * any active buffer might be error-prone. */
                if (Buffer->ListEntry.Flink != Buffer->ListEntry.Blink)
                    ConioDeleteScreenBuffer(Buffer);
            }
            else if (Object->Type == CONIO_INPUT_BUFFER_MAGIC)
            {
                DPRINT1("Closing the input buffer\n");
            }
        }

        /// LOCK /// LeaveCriticalSection(&Console->Lock);
        Entry->Object = NULL;
    }
}


/* FUNCTIONS *****************************************************************/

/* static */ NTSTATUS
FASTCALL
Win32CsrInitHandlesTable(IN OUT PCONSOLE_PROCESS_DATA ProcessData,
                         OUT PHANDLE pInputHandle,
                         OUT PHANDLE pOutputHandle,
                         OUT PHANDLE pErrorHandle)
{
    NTSTATUS Status;
    HANDLE InputHandle  = INVALID_HANDLE_VALUE,
           OutputHandle = INVALID_HANDLE_VALUE,
           ErrorHandle  = INVALID_HANDLE_VALUE;

    /*
     * Initialize the handles table. Use temporary variables to store
     * the handles values in such a way that, if we fail, we don't
     * return to the caller invalid handle values.
     *
     * Insert the IO handles.
     */

    RtlEnterCriticalSection(&ProcessData->HandleTableLock);

    /* Insert the Input handle */
    Status = Win32CsrInsertObject(ProcessData,
                                  &InputHandle,
                                  &ProcessData->Console->InputBuffer.Header,
                                  GENERIC_READ | GENERIC_WRITE,
                                  TRUE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to insert the input handle\n");
        RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
        Win32CsrFreeHandlesTable(ProcessData);
        return Status;
    }

    /* Insert the Output handle */
    Status = Win32CsrInsertObject(ProcessData,
                                  &OutputHandle,
                                  &ProcessData->Console->ActiveBuffer->Header,
                                  GENERIC_READ | GENERIC_WRITE,
                                  TRUE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to insert the output handle\n");
        RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
        Win32CsrFreeHandlesTable(ProcessData);
        return Status;
    }

    /* Insert the Error handle */
    Status = Win32CsrInsertObject(ProcessData,
                                  &ErrorHandle,
                                  &ProcessData->Console->ActiveBuffer->Header,
                                  GENERIC_READ | GENERIC_WRITE,
                                  TRUE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to insert the error handle\n");
        RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
        Win32CsrFreeHandlesTable(ProcessData);
        return Status;
    }

    /* Return the newly created handles */
    *pInputHandle  = InputHandle;
    *pOutputHandle = OutputHandle;
    *pErrorHandle  = ErrorHandle;

    RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
    return STATUS_SUCCESS;
}

NTSTATUS
FASTCALL
Win32CsrInheritHandlesTable(IN PCONSOLE_PROCESS_DATA SourceProcessData,
                            IN PCONSOLE_PROCESS_DATA TargetProcessData)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG i;

    RtlEnterCriticalSection(&SourceProcessData->HandleTableLock);

    /* Inherit a handles table only if there is no already */
    if (TargetProcessData->HandleTable != NULL /* || TargetProcessData->HandleTableSize != 0 */)
    {
        Status = STATUS_UNSUCCESSFUL; /* STATUS_INVALID_PARAMETER */
        goto Quit;
    }

    /* Allocate a new handle table for the child process */
    TargetProcessData->HandleTable = RtlAllocateHeap(ConSrvHeap,
                                                     HEAP_ZERO_MEMORY,
                                                     SourceProcessData->HandleTableSize
                                                             * sizeof(CONSOLE_IO_HANDLE));
    if (TargetProcessData->HandleTable == NULL)
    {
        Status = STATUS_NO_MEMORY;
        goto Quit;
    }

    TargetProcessData->HandleTableSize = SourceProcessData->HandleTableSize;

    /*
     * Parse the parent process' handles table and, for each handle,
     * do a copy of it and reference it, if the handle is inheritable.
     */
    for (i = 0; i < SourceProcessData->HandleTableSize; i++)
    {
        if (SourceProcessData->HandleTable[i].Object != NULL &&
            SourceProcessData->HandleTable[i].Inheritable)
        {
            /*
             * Copy the handle data and increment the reference count of the
             * pointed object (via the call to Win32CsrCreateHandleEntry).
             */
            TargetProcessData->HandleTable[i] = SourceProcessData->HandleTable[i];
            Win32CsrCreateHandleEntry(&TargetProcessData->HandleTable[i]);
        }
    }

Quit:
    RtlLeaveCriticalSection(&SourceProcessData->HandleTableLock);
    return Status;
}

VOID
FASTCALL
Win32CsrFreeHandlesTable(PCONSOLE_PROCESS_DATA ProcessData)
{
    DPRINT1("Win32CsrFreeHandlesTable\n");

    RtlEnterCriticalSection(&ProcessData->HandleTableLock);

    if (ProcessData->HandleTable != NULL)
    {
        ULONG i;

        /* Close all console handles and free the handle table memory */
        for (i = 0; i < ProcessData->HandleTableSize; i++)
        {
            Win32CsrCloseHandleEntry(&ProcessData->HandleTable[i]);
        }
        RtlFreeHeap(ConSrvHeap, 0, ProcessData->HandleTable);
        ProcessData->HandleTable = NULL;
    }

    ProcessData->HandleTableSize = 0;

    RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
}

NTSTATUS
FASTCALL
Win32CsrInsertObject(PCONSOLE_PROCESS_DATA ProcessData,
                     PHANDLE Handle,
                     Object_t *Object,
                     DWORD Access,
                     BOOL Inheritable,
                     DWORD ShareMode)
{
#define IO_HANDLES_INCREMENT    2*3

    ULONG i;
    PCONSOLE_IO_HANDLE Block;

    RtlEnterCriticalSection(&ProcessData->HandleTableLock);

    for (i = 0; i < ProcessData->HandleTableSize; i++)
    {
        if (ProcessData->HandleTable[i].Object == NULL)
        {
            break;
        }
    }
    if (i >= ProcessData->HandleTableSize)
    {
        Block = RtlAllocateHeap(ConSrvHeap,
                                HEAP_ZERO_MEMORY,
                                (ProcessData->HandleTableSize +
                                    IO_HANDLES_INCREMENT) * sizeof(CONSOLE_IO_HANDLE));
        if (Block == NULL)
        {
            RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
            return STATUS_UNSUCCESSFUL;
        }
        RtlCopyMemory(Block,
                      ProcessData->HandleTable,
                      ProcessData->HandleTableSize * sizeof(CONSOLE_IO_HANDLE));
        RtlFreeHeap(ConSrvHeap, 0, ProcessData->HandleTable);
        ProcessData->HandleTable = Block;
        ProcessData->HandleTableSize += IO_HANDLES_INCREMENT;
    }

    ProcessData->HandleTable[i].Object      = Object;
    ProcessData->HandleTable[i].Access      = Access;
    ProcessData->HandleTable[i].Inheritable = Inheritable;
    ProcessData->HandleTable[i].ShareMode   = ShareMode;
    Win32CsrCreateHandleEntry(&ProcessData->HandleTable[i]);
    *Handle = ULongToHandle((i << 2) | 0x3);

    RtlLeaveCriticalSection(&ProcessData->HandleTableLock);

    return STATUS_SUCCESS;
}

NTSTATUS
FASTCALL
Win32CsrReleaseObject(PCONSOLE_PROCESS_DATA ProcessData,
                      HANDLE Handle)
{
    ULONG_PTR h = (ULONG_PTR)Handle >> 2;
    Object_t *Object;

    RtlEnterCriticalSection(&ProcessData->HandleTableLock);

    if (h >= ProcessData->HandleTableSize ||
        (Object = ProcessData->HandleTable[h].Object) == NULL)
    {
        RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
        return STATUS_INVALID_HANDLE;
    }

    DPRINT1("Win32CsrReleaseObject - Process 0x%p, Release 0x%p\n", ProcessData->Process, &ProcessData->HandleTable[h]);
    Win32CsrCloseHandleEntry(&ProcessData->HandleTable[h]);

    RtlLeaveCriticalSection(&ProcessData->HandleTableLock);

    return STATUS_SUCCESS;
}

NTSTATUS
FASTCALL
Win32CsrLockObject(PCONSOLE_PROCESS_DATA ProcessData,
                   HANDLE Handle,
                   Object_t **Object,
                   DWORD Access,
                   BOOL LockConsole,
                   ULONG Type)
{
    ULONG_PTR h = (ULONG_PTR)Handle >> 2;

    // DPRINT("Win32CsrLockObject, Object: %x, %x, %x\n",
           // Object, Handle, ProcessData ? ProcessData->HandleTableSize : 0);

    RtlEnterCriticalSection(&ProcessData->HandleTableLock);

    if ( !IsConsoleHandle(Handle) ||
         h >= ProcessData->HandleTableSize  ||
         (*Object = ProcessData->HandleTable[h].Object) == NULL ||
         (ProcessData->HandleTable[h].Access & Access) == 0     ||
         (Type != 0 && (*Object)->Type != Type) )
    {
        DPRINT1("CsrGetObject returning invalid handle (%x) of type %lu with access %lu\n", Handle, Type, Access);
        RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
        return STATUS_INVALID_HANDLE;
    }

    _InterlockedIncrement(&(*Object)->Console->ReferenceCount);
    RtlLeaveCriticalSection(&ProcessData->HandleTableLock);

    if (LockConsole) EnterCriticalSection(&(*Object)->Console->Lock);

    return STATUS_SUCCESS;
}

VOID FASTCALL
Win32CsrUnlockConsole(PCONSOLE Console,
                      BOOL IsConsoleLocked)
{
    if (IsConsoleLocked) LeaveCriticalSection(&Console->Lock);

    /* Decrement reference count */
    if (_InterlockedDecrement(&Console->ReferenceCount) == 0)
        ConioDeleteConsole(Console);
}

VOID
FASTCALL
Win32CsrUnlockObject(Object_t *Object,
                     BOOL IsConsoleLocked)
{
    Win32CsrUnlockConsole(Object->Console, IsConsoleLocked);
}

NTSTATUS
FASTCALL
Win32CsrAllocateConsole(PCONSOLE_PROCESS_DATA ProcessData,
                        PHANDLE pInputHandle,
                        PHANDLE pOutputHandle,
                        PHANDLE pErrorHandle,
                        int ShowCmd,
                        PCSR_PROCESS CsrProcess)
{
    NTSTATUS Status = STATUS_SUCCESS;

    /* Initialize a new Console owned by the Console Leader Process */
    Status = CsrInitConsole(&ProcessData->Console, ShowCmd, CsrProcess);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Console initialization failed\n");
        return Status;
    }

    /* Initialize the handles table */
    Status = Win32CsrInitHandlesTable(ProcessData,
                                      pInputHandle,
                                      pOutputHandle,
                                      pErrorHandle);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to initialize the handles table\n");

        // Win32CsrReleaseConsole(ProcessData);
        ConioDeleteConsole(ProcessData->Console);
        ProcessData->Console = NULL;

        return Status;
    }

    return Status;
}

VOID
FASTCALL
Win32CsrReleaseConsole(PCONSOLE_PROCESS_DATA ProcessData)
{
    PCONSOLE Console;

    DPRINT1("Win32CsrReleaseConsole\n");

    /* Close all console handles and free the handle table memory */
    Win32CsrFreeHandlesTable(ProcessData);

    /* Detach process from console */
    Console = ProcessData->Console;
    if (Console != NULL)
    {
        DPRINT1("Win32CsrReleaseConsole - Console->ReferenceCount = %lu - We are going to decrement it !\n", Console->ReferenceCount);
        ProcessData->Console = NULL;
        EnterCriticalSection(&Console->Lock);
        DPRINT1("Win32CsrReleaseConsole - Locking OK\n");
        RemoveEntryList(&ProcessData->ConsoleLink);
        Win32CsrUnlockConsole(Console, TRUE);
        //CloseHandle(ProcessData->ConsoleEvent);
        //ProcessData->ConsoleEvent = NULL;
    }
}

NTSTATUS
FASTCALL
ConioConsoleFromProcessData(PCONSOLE_PROCESS_DATA ProcessData,
                            PCONSOLE* Console,
                            BOOL LockConsole)
{
    PCONSOLE ProcessConsole;

    RtlEnterCriticalSection(&ProcessData->HandleTableLock);
    ProcessConsole = ProcessData->Console;

    if (!ProcessConsole)
    {
        *Console = NULL;
        RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
        return STATUS_INVALID_HANDLE;
    }

    InterlockedIncrement(&ProcessConsole->ReferenceCount);
    RtlLeaveCriticalSection(&ProcessData->HandleTableLock);

    if (LockConsole) EnterCriticalSection(&ProcessConsole->Lock);

    *Console = ProcessConsole;

    return STATUS_SUCCESS;
}



NTSTATUS
NTAPI
ConsoleNewProcess(PCSR_PROCESS SourceProcess,
                  PCSR_PROCESS TargetProcess)
{
    /**************************************************************************
     * This function is called whenever a new process (GUI or CUI) is created.
     *
     * Copy the parent's handles table here if both the parent and the child
     * processes are CUI. If we must actually create our proper console (and
     * thus do not inherit from the console handles of the parent's), then we
     * will clean this table in the next ConsoleConnect call. Why we are doing
     * this? It's because here, we still don't know whether or not we must create
     * a new console instead of inherit it from the parent, and, because in
     * ConsoleConnect we don't have any reference to the parent process anymore.
     **************************************************************************/

    PCONSOLE_PROCESS_DATA SourceProcessData, TargetProcessData;

    DPRINT1("ConsoleNewProcess inside\n");
    DPRINT1("SourceProcess = 0x%p ; TargetProcess = 0x%p\n", SourceProcess, TargetProcess);

    /* An empty target process is invalid */
    if (!TargetProcess)
        return STATUS_INVALID_PARAMETER;

    DPRINT1("ConsoleNewProcess - OK\n");

    TargetProcessData = ConsoleGetPerProcessData(TargetProcess);
    DPRINT1("TargetProcessData = 0x%p\n", TargetProcessData);

    /**** HACK !!!! ****/ RtlZeroMemory(TargetProcessData, sizeof(*TargetProcessData));

    /* Initialize the new (target) process */
    TargetProcessData->Process = TargetProcess;
    TargetProcessData->ConsoleEvent = NULL;
    TargetProcessData->Console = TargetProcessData->ParentConsole = NULL;
    TargetProcessData->ConsoleApp = ((TargetProcess->Flags & CsrProcessIsConsoleApp) ? TRUE : FALSE);

    // Testing
    TargetProcessData->HandleTableSize = 0;
    TargetProcessData->HandleTable = NULL;

    /**** HACK !!!! ****/ RtlZeroMemory(&TargetProcessData->HandleTableLock, sizeof(RTL_CRITICAL_SECTION));
    RtlInitializeCriticalSection(&TargetProcessData->HandleTableLock);

    /* Do nothing if the source process is NULL */
    if (!SourceProcess)
        return STATUS_SUCCESS;

    SourceProcessData = ConsoleGetPerProcessData(SourceProcess);
    DPRINT1("SourceProcessData = 0x%p\n", SourceProcessData);

    /*
     * If both of the processes (parent and new child) are console applications,
     * then try to inherit handles from the parent process.
     */
    if ( SourceProcessData->Console != NULL && /* SourceProcessData->ConsoleApp */
         TargetProcessData->ConsoleApp )
    {
        NTSTATUS Status;

        Status = Win32CsrInheritHandlesTable(SourceProcessData, TargetProcessData);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        /* Temporary "inherit" the console from the parent */
        TargetProcessData->ParentConsole = SourceProcessData->Console;
    }
    else
    {
        DPRINT1("ConsoleNewProcess - We don't launch a Console process : SourceProcessData->Console = 0x%p ; TargetProcess->Flags = %lu\n", SourceProcessData->Console, TargetProcess->Flags);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
ConsoleConnect(IN PCSR_PROCESS CsrProcess,
               IN OUT PVOID ConnectionInfo,
               IN OUT PULONG ConnectionInfoLength)
{
    /**************************************************************************
     * This function is called whenever a CUI new process is created.
     **************************************************************************/

    NTSTATUS Status = STATUS_SUCCESS;
    PCONSOLE_CONNECTION_INFO ConnectInfo = (PCONSOLE_CONNECTION_INFO)ConnectionInfo;
    PCONSOLE_PROCESS_DATA ProcessData = ConsoleGetPerProcessData(CsrProcess);

    DPRINT1("ConsoleConnect\n");

    if ( ConnectionInfo       == NULL ||
         ConnectionInfoLength == NULL ||
        *ConnectionInfoLength != sizeof(CONSOLE_CONNECTION_INFO) )
    {
        DPRINT1("CONSRV: Connection failed\n");
        return STATUS_UNSUCCESSFUL;
    }

    /* If we don't need a console, then get out of here */
    if (!ConnectInfo->ConsoleNeeded || !ProcessData->ConsoleApp) // In fact, it is for GUI apps.
    {
        DPRINT("ConsoleConnect - No console needed\n");
        return STATUS_SUCCESS;
    }

    /* If we don't have a console, then create a new one... */
    if (!ConnectInfo->Console ||
         ConnectInfo->Console != ProcessData->ParentConsole)
    {
        DPRINT1("ConsoleConnect - Allocate a new console\n");

        /*
         * We are about to create a new console. However when ConsoleNewProcess
         * was called, we didn't know that we wanted to create a new console and
         * therefore, we by default inherited the handles table from our parent
         * process. It's only now that we notice that in fact we do not need
         * them, because we've created a new console and thus we must use it.
         *
         * Therefore, free the console we can have and our handles table,
         * and recreate a new one later on.
         */
        Win32CsrReleaseConsole(ProcessData);

        /* Initialize a new Console owned by the Console Leader Process */
        Status = Win32CsrAllocateConsole(ProcessData,
                                         &ConnectInfo->InputHandle,
                                         &ConnectInfo->OutputHandle,
                                         &ConnectInfo->ErrorHandle,
                                         ConnectInfo->ShowCmd,
                                         CsrProcess);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Console allocation failed\n");
            return Status;
        }
    }
    else /* We inherit it from the parent */
    {
        DPRINT1("ConsoleConnect - Reuse current (parent's) console\n");

        /* Reuse our current console */
        ProcessData->Console = ConnectInfo->Console;
    }

    /* Add a reference count because the process is tied to the console */
    _InterlockedIncrement(&ProcessData->Console->ReferenceCount);

    /* Insert the process into the processes list of the console */
    InsertHeadList(&ProcessData->Console->ProcessList, &ProcessData->ConsoleLink);

    /// TODO: Move this up ?
    /* Duplicate the Event */
    Status = NtDuplicateObject(NtCurrentProcess(),
                               ProcessData->Console->InputBuffer.ActiveEvent,
                               ProcessData->Process->ProcessHandle,
                               &ProcessData->ConsoleEvent,
                               EVENT_ALL_ACCESS, 0, 0);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("NtDuplicateObject() failed: %lu\n", Status);
        Win32CsrReleaseConsole(ProcessData);
        return Status;
    }

    /* Return it to the caller */
    ConnectInfo->Console = ProcessData->Console;

    /* Input Wait Handle */
    ConnectInfo->InputWaitHandle = ProcessData->ConsoleEvent;

    /* Set the Ctrl Dispatcher */
    ProcessData->CtrlDispatcher = ConnectInfo->CtrlDispatcher;
    DPRINT("CONSRV: CtrlDispatcher address: %x\n", ProcessData->CtrlDispatcher);

    return STATUS_SUCCESS;
}

VOID
WINAPI
ConsoleDisconnect(PCSR_PROCESS Process)
{
    PCONSOLE_PROCESS_DATA ProcessData = ConsoleGetPerProcessData(Process);

    /**************************************************************************
     * This function is called whenever a new process (GUI or CUI) is destroyed.
     **************************************************************************/

    DPRINT1("ConsoleDisconnect called\n");
    if ( ProcessData->Console     != NULL ||
         ProcessData->HandleTable != NULL )
    {
        DPRINT1("ConsoleDisconnect - calling Win32CsrReleaseConsole\n");
        Win32CsrReleaseConsole(ProcessData);
    }

    RtlDeleteCriticalSection(&ProcessData->HandleTableLock);
}



CSR_API(SrvCloseHandle)
{
    PCONSOLE_CLOSEHANDLE CloseHandleRequest = &((PCONSOLE_API_MESSAGE)ApiMessage)->Data.CloseHandleRequest;

    return Win32CsrReleaseObject(ConsoleGetPerProcessData(CsrGetClientThread()->Process),
                                 CloseHandleRequest->ConsoleHandle);
}

CSR_API(SrvVerifyConsoleIoHandle)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PCONSOLE_VERIFYHANDLE VerifyHandleRequest = &((PCONSOLE_API_MESSAGE)ApiMessage)->Data.VerifyHandleRequest;
    PCONSOLE_PROCESS_DATA ProcessData = ConsoleGetPerProcessData(CsrGetClientThread()->Process);
    HANDLE ConsoleHandle = VerifyHandleRequest->ConsoleHandle;
    ULONG_PTR Index = (ULONG_PTR)ConsoleHandle >> 2;

    RtlEnterCriticalSection(&ProcessData->HandleTableLock);

    if (!IsConsoleHandle(ConsoleHandle)    ||
        Index >= ProcessData->HandleTableSize ||
        ProcessData->HandleTable[Index].Object == NULL)
    {
        DPRINT("CsrVerifyObject failed\n");
        Status = STATUS_INVALID_HANDLE;
    }

    RtlLeaveCriticalSection(&ProcessData->HandleTableLock);

    return Status;
}

CSR_API(SrvDuplicateHandle)
{
    PCONSOLE_IO_HANDLE Entry;
    DWORD DesiredAccess;
    PCONSOLE_DUPLICATEHANDLE DuplicateHandleRequest = &((PCONSOLE_API_MESSAGE)ApiMessage)->Data.DuplicateHandleRequest;
    PCONSOLE_PROCESS_DATA ProcessData = ConsoleGetPerProcessData(CsrGetClientThread()->Process);
    HANDLE ConsoleHandle = DuplicateHandleRequest->ConsoleHandle;
    ULONG_PTR Index = (ULONG_PTR)ConsoleHandle >> 2;

    RtlEnterCriticalSection(&ProcessData->HandleTableLock);

    if ( /** !IsConsoleHandle(ConsoleHandle)    || **/
        Index >= ProcessData->HandleTableSize ||
        (Entry = &ProcessData->HandleTable[Index])->Object == NULL)
    {
        DPRINT1("Couldn't duplicate invalid handle %p\n", ConsoleHandle);
        RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
        return STATUS_INVALID_HANDLE;
    }

    if (DuplicateHandleRequest->Options & DUPLICATE_SAME_ACCESS)
    {
        DesiredAccess = Entry->Access;
    }
    else
    {
        DesiredAccess = DuplicateHandleRequest->Access;
        /* Make sure the source handle has all the desired flags */
        if ((Entry->Access & DesiredAccess) == 0)
        {
            DPRINT1("Handle %p only has access %X; requested %X\n",
                ConsoleHandle, Entry->Access, DesiredAccess);
            RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
            return STATUS_INVALID_PARAMETER;
        }
    }

    ApiMessage->Status = Win32CsrInsertObject(ProcessData,
                                              &DuplicateHandleRequest->ConsoleHandle, // Use the new handle value!
                                              Entry->Object,
                                              DesiredAccess,
                                              DuplicateHandleRequest->Inheritable,
                                              Entry->ShareMode);
    if (NT_SUCCESS(ApiMessage->Status) &&
        DuplicateHandleRequest->Options & DUPLICATE_CLOSE_SOURCE)
    {
        Win32CsrCloseHandleEntry(Entry);
    }

    RtlLeaveCriticalSection(&ProcessData->HandleTableLock);
    return ApiMessage->Status;
}

/**
CSR_API(CsrGetInputWaitHandle)
{
    PCSRSS_GET_INPUT_WAIT_HANDLE GetConsoleInputWaitHandle = &((PCONSOLE_API_MESSAGE)ApiMessage)->Data.GetConsoleInputWaitHandle;

    GetConsoleInputWaitHandle->InputWaitHandle =
        ConsoleGetPerProcessData(CsrGetClientThread()->Process)->ConsoleEvent;

    return STATUS_SUCCESS;
}
**/

/* EOF */
