/*
 * Process Hacker -
 *   qt wrapper and support functions
 *
 * Copyright (C) 2010-2015 wj32
 * Copyright (C) 2017 dmex
 * Copyright (C) 2019 David Xanatos
 *
 * This file is part of Task Explorer and contains Process Hacker code.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Process Hacker.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "stdafx.h"
#include "../../GUI/TaskExplorer.h"
#include "WinHandle.h"
#include "ProcessHacker.h"

#define PH_FILEMODE_ASYNC 0x01000000
#define PhFileModeUpdAsyncFlag(mode) \
    (mode & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT) ? mode &~ PH_FILEMODE_ASYNC: mode | PH_FILEMODE_ASYNC)

CWinHandle::CWinHandle(QObject *parent) 
	: CHandleInfo(parent) 
{
	m_Object = -1;
	m_Attributes = 0;
	m_GrantedAccess = 0;
	m_TypeIndex = 0;
	m_FileFlags = 0;
}

CWinHandle::~CWinHandle()
{
}

bool CWinHandle::InitStaticData(struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX* handle)
{
	QWriteLocker Locker(&m_Mutex);

	m_ProcessId = (quint64)handle->UniqueProcessId;

	m_HandleId = handle->HandleValue;
	m_Object = (quint64)handle->Object;
	m_Attributes = handle->HandleAttributes;
	m_GrantedAccess = handle->GrantedAccess;
	m_TypeIndex = handle->ObjectTypeIndex;

	return true;
}

bool CWinHandle::InitExtData(struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX* handle, quint64 ProcessHandle, bool bFull)
{
	QWriteLocker Locker(&m_Mutex);

	PPH_STRING TypeName;
    PPH_STRING ObjectName; // Original Name
    PPH_STRING BestObjectName; // File Name

    PhGetHandleInformationEx((HANDLE)ProcessHandle, (HANDLE)handle->HandleValue, handle->ObjectTypeIndex, 0, NULL, NULL, &TypeName, &ObjectName, &BestObjectName, NULL );

	if (bFull)
	{
		// HACK: Some security products block NtQueryObject with ObjectTypeInformation and return an invalid type
		// so we need to lookup the TypeName using the TypeIndex. We should improve PhGetHandleInformationEx for this case
		// but for now we'll preserve backwards compat by doing the lookup here. (dmex)
		if (PhIsNullOrEmptyString(TypeName))
		{
			PPH_STRING typeName;

			if (typeName = PhGetObjectTypeName(m_TypeIndex))
			{
				PhMoveReference((PVOID*)&TypeName, typeName);
			}
		}

		if (TypeName && PhEqualString2(TypeName, L"File", TRUE) && KphIsConnected())
		{
			KPH_FILE_OBJECT_INFORMATION objectInfo;

			if (NT_SUCCESS(KphQueryInformationObject((HANDLE)ProcessHandle, (HANDLE)handle->HandleValue, KphObjectFileObjectInformation, &objectInfo, sizeof(KPH_FILE_OBJECT_INFORMATION), NULL)))
			{
				if (objectInfo.SharedRead)
					m_FileFlags |= PH_HANDLE_FILE_SHARED_READ;
				if (objectInfo.SharedWrite)
					m_FileFlags |= PH_HANDLE_FILE_SHARED_WRITE;
				if (objectInfo.SharedDelete)
					m_FileFlags |= PH_HANDLE_FILE_SHARED_DELETE;
			}
		}
	}

	m_TypeName = CastPhString(TypeName);
    m_OriginalName = CastPhString(ObjectName);
    m_FileName = CastPhString(BestObjectName);

	return true;
}

QFutureWatcher<bool>* CWinHandle::InitExtDataAsync(struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX* handle, quint64 ProcessHandle)
{
	QFutureWatcher<bool>* pWatcher = new QFutureWatcher<bool>(this);
	pWatcher->setFuture(QtConcurrent::run(CWinHandle::InitExtDataAsync, this, handle, ProcessHandle));
	return pWatcher;
}

bool CWinHandle::InitExtDataAsync(CWinHandle* This, struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX* handle, quint64 ProcessHandle)
{
	return This->InitExtData(handle, ProcessHandle, false);
}

bool CWinHandle::UpdateDynamicData(struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX* handle, quint64 ProcessHandle)
{
	QWriteLocker Locker(&m_Mutex);

	BOOLEAN modified = FALSE;

	if (m_Attributes != handle->HandleAttributes)
	{
		m_Attributes = handle->HandleAttributes;
		modified = TRUE;
	}

	if (m_TypeName == "File")
    {
        HANDLE fileHandle;
        if (NT_SUCCESS(NtDuplicateObject((HANDLE)ProcessHandle, (HANDLE)m_HandleId, NtCurrentProcess(), &fileHandle, MAXIMUM_ALLOWED, 0, 0)))
        {
			QString SubTypeName;
			quint64 FileSize = 0;
			quint64 FilePosition = 0;

            BOOLEAN isFileOrDirectory = FALSE;
            BOOLEAN isConsoleHandle = FALSE;
            FILE_FS_DEVICE_INFORMATION fileDeviceInfo;
            FILE_STANDARD_INFORMATION fileStandardInfo;
            FILE_POSITION_INFORMATION filePositionInfo;
            IO_STATUS_BLOCK isb;

            if (NT_SUCCESS(NtQueryVolumeInformationFile(fileHandle, &isb, &fileDeviceInfo, sizeof(FILE_FS_DEVICE_INFORMATION), FileFsDeviceInformation)))
            {
                switch (fileDeviceInfo.DeviceType)
                {
                case FILE_DEVICE_NAMED_PIPE:
					SubTypeName = tr("Pipe");
                    break;
                case FILE_DEVICE_CD_ROM:
                case FILE_DEVICE_CD_ROM_FILE_SYSTEM:
                case FILE_DEVICE_CONTROLLER:
                case FILE_DEVICE_DATALINK:
                case FILE_DEVICE_DFS:
                case FILE_DEVICE_DISK:
                case FILE_DEVICE_DISK_FILE_SYSTEM:
                case FILE_DEVICE_VIRTUAL_DISK:
                    isFileOrDirectory = TRUE;
                    SubTypeName = tr("File or directory");
                    break;
                case FILE_DEVICE_CONSOLE:
                    isConsoleHandle = TRUE;
                    SubTypeName = tr("Console");
                    break;
                default:
                    SubTypeName = tr("Other");
                    break;
                }
            }

            if (!isConsoleHandle)
            {
                if (NT_SUCCESS(NtQueryInformationFile(fileHandle, &isb, &fileStandardInfo, sizeof(FILE_STANDARD_INFORMATION), FileStandardInformation)))
                {
					SubTypeName = fileStandardInfo.Directory ? tr("Directory") : tr("File");

					FileSize = fileStandardInfo.EndOfFile.QuadPart;
                }

				if (NT_SUCCESS(NtQueryInformationFile(fileHandle, &isb, &filePositionInfo, sizeof(FILE_POSITION_INFORMATION), FilePositionInformation)))
				{
					FilePosition = filePositionInfo.CurrentByteOffset.QuadPart;
				}
            }


			if (m_SubTypeName != SubTypeName)
			{
				m_SubTypeName = SubTypeName;
				modified = TRUE;
			}
			if (m_Size != FileSize)
			{
				m_Size = FileSize;
				modified = TRUE;
			}
			if (m_Position != FilePosition)
			{
				m_Position = FilePosition;
				modified = TRUE;
			}

            NtClose(fileHandle);
        }
    }

	return modified;
}

// hndlprv.c
/**
 * Enumerates all handles in a process.
 *
 * \param ProcessId The ID of the process.
 * \param ProcessHandle A handle to the process.
 * \param Handles A variable which receives a pointer to a buffer containing
 * information about the handles.
 * \param FilterNeeded A variable which receives a boolean indicating
 * whether the handle information needs to be filtered by process ID.
 */
NTSTATUS PhEnumHandlesGeneric(
	_In_ HANDLE ProcessId,
	_In_ HANDLE ProcessHandle,
	_Out_ PSYSTEM_HANDLE_INFORMATION_EX *Handles,
	_Out_ PBOOLEAN FilterNeeded
)
{
	NTSTATUS status;

	// There are three ways of enumerating handles:
	// * On Windows 8 and later, NtQueryInformationProcess with ProcessHandleInformation is the most efficient method.
	// * On Windows XP and later, NtQuerySystemInformation with SystemExtendedHandleInformation.
	// * Otherwise, NtQuerySystemInformation with SystemHandleInformation can be used.

	if (KphIsConnected())
	{
		PKPH_PROCESS_HANDLE_INFORMATION handles;
		PSYSTEM_HANDLE_INFORMATION_EX convertedHandles;
		ULONG i;

		// Enumerate handles using KProcessHacker. Unlike with NtQuerySystemInformation,
		// this only enumerates handles for a single process and saves a lot of processing.

		if (!NT_SUCCESS(status = KphEnumerateProcessHandles2(ProcessHandle, &handles)))
			goto FAILED;

		convertedHandles = (PSYSTEM_HANDLE_INFORMATION_EX)PhAllocate(
			FIELD_OFFSET(SYSTEM_HANDLE_INFORMATION_EX, Handles) +
			sizeof(SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX) * handles->HandleCount
		);

		convertedHandles->NumberOfHandles = handles->HandleCount;

		for (i = 0; i < handles->HandleCount; i++)
		{
			convertedHandles->Handles[i].Object = handles->Handles[i].Object;
			convertedHandles->Handles[i].UniqueProcessId = (ULONG_PTR)ProcessId;
			convertedHandles->Handles[i].HandleValue = (ULONG_PTR)handles->Handles[i].Handle;
			convertedHandles->Handles[i].GrantedAccess = (ULONG)handles->Handles[i].GrantedAccess;
			convertedHandles->Handles[i].CreatorBackTraceIndex = 0;
			convertedHandles->Handles[i].ObjectTypeIndex = handles->Handles[i].ObjectTypeIndex;
			convertedHandles->Handles[i].HandleAttributes = handles->Handles[i].HandleAttributes;
		}

		PhFree(handles);

		*Handles = convertedHandles;
		*FilterNeeded = FALSE;
	}
	else if (WindowsVersion >= WINDOWS_8 /*&& PhGetIntegerSetting(L"EnableHandleSnapshot")*/) // ToDo: add settings
	{
		PPROCESS_HANDLE_SNAPSHOT_INFORMATION handles;
		PSYSTEM_HANDLE_INFORMATION_EX convertedHandles;
		ULONG i;

		if (!NT_SUCCESS(status = PhEnumHandlesEx2(ProcessHandle, &handles)))
			goto FAILED;

		convertedHandles = (PSYSTEM_HANDLE_INFORMATION_EX)PhAllocate(
			FIELD_OFFSET(SYSTEM_HANDLE_INFORMATION_EX, Handles) +
			sizeof(SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX) * handles->NumberOfHandles
		);

		convertedHandles->NumberOfHandles = handles->NumberOfHandles;

		for (i = 0; i < handles->NumberOfHandles; i++)
		{
			convertedHandles->Handles[i].Object = 0;
			convertedHandles->Handles[i].UniqueProcessId = (ULONG_PTR)ProcessId;
			convertedHandles->Handles[i].HandleValue = (ULONG_PTR)handles->Handles[i].HandleValue;
			convertedHandles->Handles[i].GrantedAccess = handles->Handles[i].GrantedAccess;
			convertedHandles->Handles[i].CreatorBackTraceIndex = 0;
			convertedHandles->Handles[i].ObjectTypeIndex = (USHORT)handles->Handles[i].ObjectTypeIndex;
			convertedHandles->Handles[i].HandleAttributes = handles->Handles[i].HandleAttributes;
		}

		PhFree(handles);

		*Handles = convertedHandles;
		*FilterNeeded = FALSE;
	}
	else
	{
		PSYSTEM_HANDLE_INFORMATION_EX handles;
	FAILED:
		if (!NT_SUCCESS(status = PhEnumHandlesEx(&handles)))
			return status;

		*Handles = handles;
		*FilterNeeded = TRUE;
	}

	return status;
}

QString CWinHandle::GetAttributesString() const
{
	QReadLocker Locker(&m_Mutex);

    switch (m_Attributes & (OBJ_PROTECT_CLOSE | OBJ_INHERIT))
    {
	case OBJ_PROTECT_CLOSE:					return tr("Protected");
    case OBJ_INHERIT:						return tr("Inherit");
    case OBJ_PROTECT_CLOSE | OBJ_INHERIT:	return tr("Protected, Inherit");
    }
	return "";
}

QString CWinHandle::GetFileShareAccessString() const
{
	QReadLocker Locker(&m_Mutex);

	QString Str = "---";
    if (m_FileFlags & PH_HANDLE_FILE_SHARED_MASK)
    {
		if (m_FileFlags & PH_HANDLE_FILE_SHARED_READ)
			Str[0] = 'R';
        if (m_FileFlags & PH_HANDLE_FILE_SHARED_WRITE)
            Str[1] = 'W';
        if (m_FileFlags & PH_HANDLE_FILE_SHARED_DELETE)
            Str[2] = 'D';
    }
	return Str;
}

QString CWinHandle::GetTypeString() const
{ 
	QReadLocker Locker(&m_Mutex); 
	if (m_SubTypeName.isEmpty())
		return m_TypeName;
	return m_TypeName + " (" + m_SubTypeName + ")"; 
}

QString CWinHandle::GetGrantedAccessString() const
{
	QReadLocker Locker(&m_Mutex);

	PPH_STRING GrantedAccessSymbolicText = NULL;
	PPH_ACCESS_ENTRY accessEntries;
	ULONG numberOfAccessEntries;
	PPH_STRING TypeName = CastQString(m_TypeName);
	if (PhGetAccessEntries(PhGetStringOrEmpty(TypeName), &accessEntries, &numberOfAccessEntries))
	{
		GrantedAccessSymbolicText = PhGetAccessString(m_GrantedAccess, accessEntries, numberOfAccessEntries);
		PhFree(accessEntries);
	}
	if(TypeName)
		PhDereferenceObject(TypeName);

	return CastPhString(GrantedAccessSymbolicText);
}

PH_ACCESS_ENTRY FileModeAccessEntries[6] = 
{
    { L"FILE_FLAG_OVERLAPPED", PH_FILEMODE_ASYNC, FALSE, FALSE, L"Asynchronous" },
    { L"FILE_FLAG_WRITE_THROUGH", FILE_WRITE_THROUGH, FALSE, FALSE, L"Write through" },
    { L"FILE_FLAG_SEQUENTIAL_SCAN", FILE_SEQUENTIAL_ONLY, FALSE, FALSE, L"Sequental" },
    { L"FILE_FLAG_NO_BUFFERING", FILE_NO_INTERMEDIATE_BUFFERING, FALSE, FALSE, L"No buffering" },
    { L"FILE_SYNCHRONOUS_IO_ALERT", FILE_SYNCHRONOUS_IO_ALERT, FALSE, FALSE, L"Synchronous alert" },
    { L"FILE_SYNCHRONOUS_IO_NONALERT", FILE_SYNCHRONOUS_IO_NONALERT, FALSE, FALSE, L"Synchronous non-alert" },
};

QVariantMap CWinHandle::GetDetailedInfos() const
{
	QVariantMap Details;

    HANDLE processHandle;
	if (!NT_SUCCESS(PhOpenProcess(&processHandle, PROCESS_DUP_HANDLE, (HANDLE)m_ProcessId)))
		return Details;
	

	OBJECT_BASIC_INFORMATION basicInfo;
	if (NT_SUCCESS(PhGetHandleInformation(processHandle, (HANDLE)m_HandleId, ULONG_MAX, &basicInfo, NULL, NULL, NULL)))
	{
		QVariantMap Refs;
		Refs[tr("References")] = (quint64)basicInfo.PointerCount;
		Refs[tr("Handles")] = (quint64)basicInfo.HandleCount;
		Details[tr("References")] = Refs;

		QVariantMap Quota;
		Quota[tr("Paged")] = (quint64)basicInfo.PagedPoolCharge;
		Quota[tr("Virtual size")] = (quint64)basicInfo.NonPagedPoolCharge;
		Details[tr("Quota charges")] = Quota;

	}

	if (m_TypeName.isEmpty())
    {
        NOTHING;
    }
    else if (m_TypeName == "ALPC Port")
    {
		QVariantMap Port;

        HANDLE alpcPortHandle;
        if (NT_SUCCESS(NtDuplicateObject(processHandle, (HANDLE)m_HandleId, NtCurrentProcess(), &alpcPortHandle, READ_CONTROL, 0, 0 )))
        {
            ALPC_BASIC_INFORMATION alpcInfo;
            if (NT_SUCCESS(NtAlpcQueryInformation(alpcPortHandle, AlpcBasicInformation, &alpcInfo, sizeof(ALPC_BASIC_INFORMATION), NULL)))
            {
				Port[tr("Sequence Number")] = (quint64)alpcInfo.SequenceNo;
				Port[tr("Port Context")] = (quint64)alpcInfo.PortContext; // L"0x%Ix"
            }

            NtClose(alpcPortHandle);
        }

		Details[tr("ALPC Port")] = Port;
    }
    else if (m_TypeName == "File")
    {
		QVariantMap File;

        HANDLE fileHandle;
        if (NT_SUCCESS(NtDuplicateObject(processHandle, (HANDLE)m_HandleId, NtCurrentProcess(), &fileHandle, MAXIMUM_ALLOWED, 0, 0)))
        {
            BOOLEAN isFileOrDirectory = FALSE;
            BOOLEAN isConsoleHandle = FALSE;
            BOOLEAN isPipeHandle = FALSE;
            FILE_FS_DEVICE_INFORMATION fileDeviceInfo;
            FILE_MODE_INFORMATION fileModeInfo;
            FILE_STANDARD_INFORMATION fileStandardInfo;
            FILE_POSITION_INFORMATION filePositionInfo;
            IO_STATUS_BLOCK isb;

            if (NT_SUCCESS(NtQueryVolumeInformationFile(fileHandle, &isb, &fileDeviceInfo, sizeof(FILE_FS_DEVICE_INFORMATION), FileFsDeviceInformation)))
            {
                switch (fileDeviceInfo.DeviceType)
                {
                case FILE_DEVICE_NAMED_PIPE:
                    isPipeHandle = TRUE;
					File["Type"] = tr("Pipe");
                    break;
                case FILE_DEVICE_CD_ROM:
                case FILE_DEVICE_CD_ROM_FILE_SYSTEM:
                case FILE_DEVICE_CONTROLLER:
                case FILE_DEVICE_DATALINK:
                case FILE_DEVICE_DFS:
                case FILE_DEVICE_DISK:
                case FILE_DEVICE_DISK_FILE_SYSTEM:
                case FILE_DEVICE_VIRTUAL_DISK:
                    isFileOrDirectory = TRUE;
					File["Type"] = tr("File or directory");
                    break;
                case FILE_DEVICE_CONSOLE:
                    isConsoleHandle = TRUE;
					File["Type"] = tr("Other");
                    break;
                default:
                    break;
                }
            }

			NTSTATUS status;
            if (isPipeHandle || isConsoleHandle)
            {
                // NOTE: NtQueryInformationFile for '\Device\ConDrv\CurrentIn' causes a deadlock but
                // we can query other '\Device\ConDrv' console handles. NtQueryInformationFile also
                // causes a deadlock for some types of named pipes and only on Win10 (dmex)
                status = PhCallNtQueryFileInformationWithTimeout(fileHandle, FileModeInformation, &fileModeInfo, sizeof(FILE_MODE_INFORMATION));
            }
            else
            {
                status = NtQueryInformationFile(fileHandle, &isb, &fileModeInfo, sizeof(FILE_MODE_INFORMATION), FileModeInformation);
            }

            if (NT_SUCCESS(status))
            {
                PH_FORMAT format[5];
                PPH_STRING fileModeAccessStr;
                WCHAR fileModeString[MAX_PATH];

                // Since FILE_MODE_INFORMATION has no flag for asynchronous I/O we should use our own flag and set
                // it only if none of synchronous flags are present. That's why we need PhFileModeUpdAsyncFlag.
                fileModeAccessStr = PhGetAccessString(PhFileModeUpdAsyncFlag(fileModeInfo.Mode), FileModeAccessEntries, RTL_NUMBER_OF(FileModeAccessEntries));

				File["Mode"] = "0x" + QString::number(fileModeInfo.Mode, 16) + " (" + CastPhString(fileModeAccessStr) + ")";
            }

            if (!isConsoleHandle)
            {
                if (NT_SUCCESS(NtQueryInformationFile(fileHandle, &isb, &fileStandardInfo, sizeof(FILE_STANDARD_INFORMATION), FileStandardInformation)))
                {
					File["Type"] = fileStandardInfo.Directory ? tr("Directory") : tr("File");
					File["Size"] = fileStandardInfo.EndOfFile.QuadPart; // format
                }

                if (NT_SUCCESS(NtQueryInformationFile(fileHandle, &isb, &filePositionInfo, sizeof(FILE_POSITION_INFORMATION), FilePositionInformation)))
                {
					File["Position"] = filePositionInfo.CurrentByteOffset.QuadPart; // format
                }
            }

            NtClose(fileHandle);
        }

		Details[tr("File information")] = File;
    }
    else if (m_TypeName == "Section")
    {
		QVariantMap Section;
		
        HANDLE sectionHandle;
        NTSTATUS status = NtDuplicateObject(processHandle, (HANDLE)m_HandleId, NtCurrentProcess(), &sectionHandle, SECTION_QUERY | SECTION_MAP_READ, 0, 0 );
        if (!NT_SUCCESS(status))
            status = NtDuplicateObject(processHandle, (HANDLE)m_HandleId, NtCurrentProcess(), &sectionHandle, SECTION_QUERY, 0, 0);

        if (NT_SUCCESS(status))
        {
            SECTION_BASIC_INFORMATION sectionInfo;
            Section[tr("Type")] = tr("Unknown");
            
			quint64 SectionSize = 0;
            if (NT_SUCCESS(PhGetSectionBasicInformation(sectionHandle, &sectionInfo)))
            {
                if (sectionInfo.AllocationAttributes & SEC_COMMIT)
                    Section[tr("Type")] = tr("Commit");
                else if (sectionInfo.AllocationAttributes & SEC_FILE)
                    Section[tr("Type")] = tr("File");
                else if (sectionInfo.AllocationAttributes & SEC_IMAGE)
                    Section[tr("Type")] = tr("Image");
                else if (sectionInfo.AllocationAttributes & SEC_RESERVE)
                    Section[tr("Type")] = tr("Reserve");

                Section[tr("Size")] = sectionInfo.MaximumSize.QuadPart;
            }

			/*PPH_STRING fileName = NULL;
            if (NT_SUCCESS(PhGetSectionFileName(sectionHandle, &fileName)))
            {
                PPH_STRING newFileName;

                PH_AUTO(fileName);

                if (newFileName = PhResolveDevicePrefix(fileName))
                    fileName = PH_AUTO(newFileName);
            }*/

			//Section[tr("File")] = ;

            NtClose(sectionHandle);
        }

		Details[tr("Section information")] = Section;
    }
    else if (m_TypeName == "Mutant")
    {
		QVariantMap Mutant;

        HANDLE mutantHandle;
        if (NT_SUCCESS(NtDuplicateObject(processHandle, (HANDLE)m_HandleId, NtCurrentProcess(), &mutantHandle, SEMAPHORE_QUERY_STATE, 0, 0)))
        {
            MUTANT_BASIC_INFORMATION mutantInfo;
            MUTANT_OWNER_INFORMATION ownerInfo;

            if (NT_SUCCESS(PhGetMutantBasicInformation(mutantHandle, &mutantInfo)))
            {
				Mutant[tr("Count")] = mutantInfo.CurrentCount;
				Mutant[tr("Abandoned")] = mutantInfo.AbandonedState ? tr("True") : tr("False");
            }

            if (NT_SUCCESS(PhGetMutantOwnerInformation(mutantHandle, &ownerInfo)))
            {
                PPH_STRING name;
                if (ownerInfo.ClientId.UniqueProcess)
                {
                    name = PhStdGetClientIdName(&ownerInfo.ClientId);
					Mutant[tr("Owner")] = CastPhString(name);
                }
            }

            NtClose(mutantHandle);
        }

		Details[tr("Mutant information")] = Mutant;
    }
    else if (m_TypeName == "Process")
    {
		QVariantMap Process;

        HANDLE dupHandle;
        if (NT_SUCCESS(NtDuplicateObject(processHandle, (HANDLE)m_HandleId, NtCurrentProcess(), &dupHandle, PROCESS_QUERY_LIMITED_INFORMATION, 0, 0)))
        {
			PPH_STRING fileName;
            if (NT_SUCCESS(PhGetProcessImageFileName(dupHandle, &fileName)))
            {
				Process["Name"] = CastPhString(fileName);
            }

			NTSTATUS exitStatus = STATUS_PENDING;
			PROCESS_BASIC_INFORMATION procInfo;
            if (NT_SUCCESS(PhGetProcessBasicInformation(dupHandle, &procInfo)))
            {
                exitStatus = procInfo.ExitStatus;

				Process["Exit status"] = exitStatus;
            }

			KERNEL_USER_TIMES times;
            if (NT_SUCCESS(PhGetProcessTimes(dupHandle, &times)))
            {
				Process["Created"] = QDateTime::fromTime_t((int64_t)times.CreateTime.QuadPart / 10000000ULL - 11644473600ULL);
				if (exitStatus != STATUS_PENDING)
					Process["Exited"] = QDateTime::fromTime_t((int64_t)times.ExitTime.QuadPart / 10000000ULL - 11644473600ULL);
            }

            NtClose(dupHandle);
        }

		Details[tr("Process information")] = Process;
    }
    else if (m_TypeName == "Thread")
    {
		QVariantMap Thread;

        HANDLE dupHandle;
        if (NT_SUCCESS(NtDuplicateObject(processHandle, (HANDLE)m_HandleId, NtCurrentProcess(), &dupHandle, THREAD_QUERY_LIMITED_INFORMATION, 0, 0)))
        {
			PPH_STRING name;
            if (NT_SUCCESS(PhGetThreadName(dupHandle, &name)))
            {
                Thread["Name"] = CastPhString(name);
            }

			NTSTATUS exitStatus = STATUS_PENDING;
			THREAD_BASIC_INFORMATION threadInfo;
            if (NT_SUCCESS(PhGetThreadBasicInformation(dupHandle, &threadInfo)))
            {
                exitStatus = threadInfo.ExitStatus;

                //if (NT_SUCCESS(PhOpenProcess(
                //    &processHandle,
                //    PROCESS_QUERY_LIMITED_INFORMATION,
                //    threadInfo.ClientId.UniqueProcess
                //    )))
                //{
                //    if (NT_SUCCESS(PhGetProcessImageFileName(processHandle, &fileName)))
                //    {
                //        PhMoveReference(&fileName, PhGetFileName(fileName));
                //        PhSetListViewSubItem(Context->ListViewHandle, Context->ListViewRowCache[PH_HANDLE_GENERAL_INDEX_PROCESSTHREADNAME], 1, PhGetStringOrEmpty(fileName));
                //        PhDereferenceObject(fileName);
                //    }
                //
                //    NtClose(processHandle);
                //}

				Thread["Exit status"] = exitStatus;
            }

			            KERNEL_USER_TIMES times;
            if (NT_SUCCESS(PhGetThreadTimes(dupHandle, &times)))
            {
				Thread["Created"] = QDateTime::fromTime_t((int64_t)times.CreateTime.QuadPart / 10000000ULL - 11644473600ULL);
				if (exitStatus != STATUS_PENDING)
					Thread["Exited"] = QDateTime::fromTime_t((int64_t)times.ExitTime.QuadPart / 10000000ULL - 11644473600ULL);
            }

            NtClose(dupHandle);
        }

		Details[tr("Thread information")] = Thread;
    }

	NtClose(processHandle);

	return Details;
}