// windows_events.cpp : Defines the entry point for the application.
//
// In VS 2015 x64 Native prompt:
// CL.exe /Zi /nologo /W3 /GS- /Od /D _DEBUG /D WIN32 /D _UNICODE /D UNICODE /Gm /EHsc /MDd /fp:precise /Zc:wchar_t /Zc:forScope /Gd /TP /await windows_events.cpp
//

#define STRICT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <ole2.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shellapi.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Ole32.lib")

#include <new>
#include <utility>
#include <memory>
#include <type_traits>
#include <tuple>

#include <experimental/resumable>
#include <experimental/generator>
namespace ex = std::experimental;

#include "unwinder.h"

#include "windows_user.h"
namespace wu = windows_user;

#include "async_generator.h"
#include "async_operators.h"
namespace ao = async::operators;
#include "async_subjects.h"
namespace asub = async::subjects;
#include "async_windows_user.h"
namespace awu = async::windows_user;

struct RootWindow : public awu::async_messages, public awu::enable_send_call<RootWindow, WM_USER+1>
{
    // window class
    using window_class = wu::window_class<RootWindow>;
    static LPCWSTR class_name() {return L"Scratch";}
    static void change_class(WNDCLASSEX& wcex) {}

    // createstruct parameter type
    using param_type = std::wstring;

    // public methods
    static LRESULT set_title(HWND w, const std::wstring& t) {
        return send_call(w, [&](RootWindow& r){
            r.set_title(t);
            return 0;
        });
    }
    void set_title(const std::wstring& t) {
        title = t;
    }

    ~RootWindow() {
        PostQuitMessage(0);
    }

    RootWindow(HWND w, LPCREATESTRUCT, param_type* title) : window(w), title(title ? *title : L"RootWindow") {
        OnPaint();
        OnPrintClient();
        OnKeyDown();
    }

private:
    // implementation

    HWND window;
    std::wstring title;

    void PaintContent(PAINTSTRUCT& ) {}

    auto OnKeyDown() -> std::future<void> {
        for __await (auto& m : messages<WM_KEYDOWN>()) {
            m.handled(); // skip DefWindowProc

            MessageBox(window, L"KeyDown", title.c_str(), MB_OK);
        }
    }

    auto OnPaint() -> std::future<void> {
        for __await (auto& m : messages<WM_PAINT>()) {
            m.handled(); // skip DefWindowProc

            PAINTSTRUCT ps;
            BeginPaint(window, &ps);
            PaintContent(ps);
            EndPaint(window, &ps);
        }
    }

    auto OnPrintClient() -> std::future<void> {
        for __await (auto& m : messages<WM_PRINTCLIENT, HDC>()) {
            m.handled(); // skip DefWindowProc

            PAINTSTRUCT ps;
            ps.hdc = m.wParam;
            GetClientRect(window, &ps.rcPaint);
            PaintContent(ps);
        }
    }
};

int PASCAL
wWinMain(HINSTANCE hinst, HINSTANCE, LPWSTR, int nShowCmd)
{
    MessageBox(nullptr, L"wwinmain", L"", MB_OK);

    HRESULT hr = S_OK;

    hr = CoInitialize(NULL);
    if (FAILED(hr))
    {
        return FALSE;
    }
    ON_UNWIND_AUTO([&]{CoUninitialize();});

    InitCommonControls();

    RootWindow::window_class::Register();

    LONG winerror = 0;

    std::wstring title{L"Scratch App - RootWindow"};

    HWND window = CreateWindow(
        RootWindow::window_class::Name(), title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        NULL, NULL,
        hinst,
        &title);
    if (!window) {winerror = GetLastError();}

    if (!!winerror || !window)
    {
        return winerror;
    }

    ShowWindow(window, nShowCmd);

    auto settitle = std::async([window](){
        RootWindow::set_title(window, L"SET! Scratch App - RootWindow");
    });

    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    settitle.get();

    return 0;
}
