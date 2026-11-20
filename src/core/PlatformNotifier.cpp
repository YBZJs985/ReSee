#include "core/PlatformNotifier.h"
#include "core/Logger.h"

#ifdef SYNC_PLATFORM_WINDOWS
#include <windows.h>
#include <shellapi.h>
#include <cstring>
#include <thread>
#endif

namespace mcs {

void PlatformNotifier::showSystemNotification(const QString& title, const QString& message, bool isError) {
#ifdef SYNC_PLATFORM_WINDOWS
    // 在独立短生命周期后台线程弹出轻量 Win32 托盘气泡通知，不阻塞主界面
    const std::wstring wTitle = title.left(60).toStdWString();
    const std::wstring wMsg = message.left(240).toStdWString();
    std::thread([wTitle, wMsg, isError]() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ReSeeNotifyDummyClass";
        RegisterClassExW(&wc);

        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"ReSeeNotify", 0,
                                    0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (!hwnd) return;

        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = 1001;
        nid.uFlags = NIF_ICON | NIF_TIP | NIF_INFO;
        nid.hIcon = LoadIconW(nullptr, isError ? IDI_WARNING : IDI_INFORMATION);
        wcsncpy_s(nid.szTip, L"ReSee 录像健康监测", _TRUNCATE);
        wcsncpy_s(nid.szInfoTitle, wTitle.c_str(), _TRUNCATE);
        wcsncpy_s(nid.szInfo, wMsg.c_str(), _TRUNCATE);
        nid.dwInfoFlags = isError ? NIIF_WARNING : NIIF_INFO;
        nid.uTimeout = 6000;

        Shell_NotifyIconW(NIM_ADD, &nid);
        Sleep(6500);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        DestroyWindow(hwnd);
    }).detach();
#else
    MCS_LOG_INFO("NOTIFY", QStringLiteral("%1: %2").arg(title, message));
#endif
}

} // namespace mcs
