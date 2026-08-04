// Main.cpp — DLL 入口。
//
// 用静态 CRT（RuntimeLibrary=MultiThreaded）时，链接器默认入口 _DllMainCRTStartup
// 会调用这里的 DllMain。我们不需要在此安装 hook——Syringe 在进程启动时读取本 DLL
// 的 .syhks00 段（由 DEFINE_HOOK 写入）自行完成 patch。
//
// 模式取自 CnCNet/yrpp-spawner 的 Main.Hook.cpp。

#include <windows.h>

BOOL __stdcall DllMain(HANDLE /*hInstance*/, DWORD dwReason, LPVOID /*reserved*/)
{
    switch (dwReason) {
    case DLL_PROCESS_ATTACH:
    case DLL_PROCESS_DETACH:
    default:
        break;
    }
    return TRUE;
}
