#include <windows.h>
#include "App.h"

namespace
{
    constexpr UINT kWidth = 1280;
    constexpr UINT kHeight = 720;
    constexpr wchar_t kWindowClassName[] = L"D3D12DXRRayTracingWindowClass";
    constexpr wchar_t kWindowTitle[] = L"D3D12 Tutorial - 16. DXRRayTracing";
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    // First keyboard input in the tutorial - steps 1-15 all ran a fixed
    // animation with nothing to press. The window's user data holds the
    // App pointer, stashed there by wWinMain once the app exists.
    case WM_KEYDOWN:
    {
        App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (app != nullptr)
        {
            app->OnKeyDown(wParam);
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClassName;
    RegisterClassEx(&wc);

    RECT windowRect = { 0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight) };
    AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowW(
        kWindowClassName, kWindowTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
        nullptr, nullptr, hInstance, nullptr);

    try
    {
        App app(hwnd, kWidth, kHeight);
        // WndProc is a free function, so this is how it reaches the app to
        // forward key presses. Cleared again below before `app` goes out
        // of scope, so no message arriving during teardown can follow a
        // dangling pointer.
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));

        ShowWindow(hwnd, nCmdShow);

        MSG msg = {};
        while (msg.message != WM_QUIT)
        {
            if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            else
            {
                app.Render();
            }
        }

        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return static_cast<int>(msg.wParam);
    }
    catch (const std::exception& e)
    {
        MessageBoxA(hwnd, e.what(), "D3D12 Tutorial - Error", MB_OK | MB_ICONERROR);
        return -1;
    }
}
