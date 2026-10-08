#pragma once
#include <Windows.h>
#include <mutex>
#include <unordered_map>
namespace NativeFinalOutput
{
// 只有私有 DX12 创建范围内、确实持有原生队列的输出 wrapper 登记。
// 以窗口为键，避免外层 Streamline 代理缺少我们私有 IID 时误建重复 runtime。
inline std::mutex mutex;
inline std::unordered_map<HWND,unsigned> outputs;
inline std::unordered_map<HWND,unsigned> legacyOutputs;
inline void Register(HWND window,bool legacy=false){std::lock_guard lock(mutex);++outputs[window];if(legacy)++legacyOutputs[window];}
inline void Unregister(HWND window,bool legacy=false){std::lock_guard lock(mutex);auto it=outputs.find(window);if(it!=outputs.end() && --it->second==0)outputs.erase(it);if(legacy){auto found=legacyOutputs.find(window);if(found!=legacyOutputs.end() && --found->second==0)legacyOutputs.erase(found);}}
inline bool ContainsLegacy(HWND window){std::lock_guard lock(mutex);return legacyOutputs.find(window)!=legacyOutputs.end();}
inline bool Contains(HWND window){std::lock_guard lock(mutex);return outputs.find(window)!=outputs.end();}
}
