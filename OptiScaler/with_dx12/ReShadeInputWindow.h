#pragma once
#include <Windows.h>
#include <cwchar>
namespace ReShadeInputWindow
{
inline bool UseHiddenSourceWindow(void* caller, bool verifiedNativeOutput = false)
{
    HMODULE gimi=GetModuleHandleW(L"d3d11.dll");
    const bool paired=gimi && GetProcAddress(gimi,"XXMIPrivateDx12PassthroughVersion")!=nullptr;
    const bool legacy=verifiedNativeOutput;
    if(!paired && !legacy) return false;
    HMODULE module=nullptr;
    if(!caller || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(caller),&module)) return false;
    wchar_t path[MAX_PATH]={}; if(!GetModuleFileNameW(module,path,MAX_PATH)) return false;
    const wchar_t* slash=std::wcsrchr(path,static_cast<wchar_t>(92));
    return _wcsicmp(slash?slash+1:path,L"ReShade64.dll")==0;
}
}
