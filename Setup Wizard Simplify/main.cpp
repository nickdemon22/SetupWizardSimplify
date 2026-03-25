#include <windows.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <process.h>    
#include <commctrl.h>   
#include <comutil.h>
#include <shldisp.h>
#include <shlguid.h>
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

using namespace Gdiplus;

// --- ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ---
HWND g_hProgressBar = nullptr;
HWND g_hEditPath = nullptr;    // Поле ввода пути
std::wstring g_AppName = L"Otter Browser";
std::wstring g_DefaultPath = L"C:\\Program Files (x86)\\Otter-Browser)"; // Путь по умолчанию
std::wstring g_MainExeName = L"otter-browser.exe";

ULONG_PTR g_gdiplusToken;
Image* g_pBgImage = nullptr;

struct InstallParams {
    HWND hwnd;
    std::wstring path;
};

// --- ФУНКЦИИ ВЗАИМОДЕЙСТВИЯ ---

// Вызов окна выбора папки
void BrowseFolder(HWND hwnd) {
    wchar_t szDir[MAX_PATH];
    BROWSEINFOW bi = { 0 };
    bi.hwndOwner = hwnd;
    bi.lpszTitle = L"Выберите папку для установки:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl != 0) {
        SHGetPathFromIDListW(pidl, szDir);
        SetWindowTextW(g_hEditPath, szDir); // Обновляем текст в поле ввода
        CoTaskMemFree(pidl);
    }
}

// --- ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ УСТАНОВКИ (как в прошлый раз) ---
Image* LoadImageFromResource(HMODULE hMod, WORD resID, const wchar_t* resType) {
    HRSRC hRes = FindResource(hMod, MAKEINTRESOURCE(resID), resType);
    if (!hRes) return nullptr;
    DWORD resSize = SizeofResource(hMod, hRes);
    HGLOBAL hResData = LoadResource(hMod, hRes);
    void* pResBuffer = LockResource(hResData);
    HGLOBAL hBuffer = GlobalAlloc(GMEM_MOVEABLE, resSize);
    void* pBuffer = GlobalLock(hBuffer);
    CopyMemory(pBuffer, pResBuffer, resSize);
    GlobalUnlock(hBuffer);
    IStream* pStream = nullptr;
    if (CreateStreamOnHGlobal(hBuffer, TRUE, &pStream) == S_OK) return Image::FromStream(pStream);
    return nullptr;
}

bool ExtractPayload(std::wstring targetFolder) {
    SHCreateDirectoryExW(NULL, targetFolder.c_str(), NULL);
    std::wstring outPath = targetFolder + L"\\payload.zip";
    HRSRC hRes = FindResource(NULL, MAKEINTRESOURCE(IDR_INSTALLER_DATA), RT_RCDATA);
    if (!hRes) return false;
    DWORD size = SizeofResource(NULL, hRes);
    HGLOBAL hData = LoadResource(NULL, hRes);
    void* pData = LockResource(hData);
    HANDLE hFile = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written;
    WriteFile(hFile, pData, size, &written, NULL);
    CloseHandle(hFile);
    return true;
}

bool UnzipResource(std::wstring zipFile, std::wstring destFolder) {
    bool success = false;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        IShellDispatch* pShell = nullptr;
        hr = CoCreateInstance(CLSID_Shell, NULL, CLSCTX_INPROC_SERVER, IID_IShellDispatch, (void**)&pShell);
        if (SUCCEEDED(hr)) {
            Folder* pDestFolder = nullptr;
            BSTR bstrDest = SysAllocString(destFolder.c_str());
            hr = pShell->NameSpace(VARIANT{ .vt = VT_BSTR, .bstrVal = bstrDest }, &pDestFolder);
            if (SUCCEEDED(hr) && pDestFolder) {
                Folder* pZipFile = nullptr;
                BSTR bstrZip = SysAllocString(zipFile.c_str());
                hr = pShell->NameSpace(VARIANT{ .vt = VT_BSTR, .bstrVal = bstrZip }, &pZipFile);
                if (SUCCEEDED(hr) && pZipFile) {
                    FolderItems* pFiles = nullptr;
                    hr = pZipFile->Items(&pFiles);
                    if (SUCCEEDED(hr) && pFiles) {
                        VARIANT vOptions; vOptions.vt = VT_I4; vOptions.lVal = 4 | 16;
                        VARIANT vItems; vItems.vt = VT_DISPATCH; vItems.pdispVal = pFiles;
                        hr = pDestFolder->CopyHere(vItems, vOptions);
                        if (SUCCEEDED(hr)) success = true;
                        pFiles->Release();
                    }
                    pZipFile->Release();
                }
                SysFreeString(bstrZip);
                pDestFolder->Release();
            }
            SysFreeString(bstrDest);
            pShell->Release();
        }
        CoUninitialize();
    }
    return success;
}

void CreateDesktopShortcut(std::wstring targetExePath, std::wstring appName) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IShellLinkW* psl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (LPVOID*)&psl))) {
        IPersistFile* ppf = nullptr;
        psl->SetPath(targetExePath.c_str());
        if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, (LPVOID*)&ppf))) {
            wchar_t desktopPath[MAX_PATH];
            SHGetSpecialFolderPathW(NULL, desktopPath, CSIDL_DESKTOPDIRECTORY, FALSE);
            std::wstring shortcutPath = std::wstring(desktopPath) + L"\\" + appName + L".lnk";
            ppf->Save(shortcutPath.c_str(), TRUE);
            ppf->Release();
        }
        psl->Release();
    }
    CoUninitialize();
}

// --- ПОТОК УСТАНОВКИ ---
unsigned __stdcall InstallThread(void* pArguments) {
    InstallParams* params = (InstallParams*)pArguments;
    SendMessage(g_hProgressBar, PBM_SETPOS, 5, 0);

    if (ExtractPayload(params->path)) {
        SendMessage(g_hProgressBar, PBM_SETPOS, 30, 0);
        std::wstring tempZip = params->path + L"\\payload.zip";
        if (UnzipResource(tempZip, params->path)) {
            SendMessage(g_hProgressBar, PBM_SETPOS, 80, 0);
            DeleteFileW(tempZip.c_str());
            if (SendMessage(GetDlgItem(params->hwnd, 100), BM_GETCHECK, 0, 0) == BST_CHECKED) {
                CreateDesktopShortcut(params->path + L"\\" + g_MainExeName, g_AppName);
            }
            SendMessage(g_hProgressBar, PBM_SETPOS, 100, 0);
            MessageBoxW(params->hwnd, L"Установка успешно завершена!", L"Готово", MB_OK | MB_ICONINFORMATION);
        }
    }
    EnableWindow(GetDlgItem(params->hwnd, 101), TRUE);
    delete params;
    return 0;
}

// --- ОКНО ---
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_PROGRESS_CLASS };
        InitCommonControlsEx(&icex);

        // Поле ввода пути (Edit)
        g_hEditPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_DefaultPath.c_str(),
            WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL,
            20, 135, 300, 25, hwnd, NULL, NULL, NULL);

        // Кнопка Обзор (...)
        CreateWindowW(L"BUTTON", L"...", WS_VISIBLE | WS_CHILD,
            330, 135, 55, 25, hwnd, (HMENU)102, NULL, NULL);

        // Чекбокс
        CreateWindowW(L"BUTTON", L"Create Desktop shortcut", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
            20, 175, 200, 20, hwnd, (HMENU)100, NULL, NULL);

        // Кнопка INSTALL
        CreateWindowW(L"BUTTON", L"INSTALL", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
            20, 205, 100, 30, hwnd, (HMENU)101, NULL, NULL);

        // Прогресс-бар
        g_hProgressBar = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            20, 250, 365, 20, hwnd, NULL, NULL, NULL);

        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        Graphics graphics(hdc);
        if (g_pBgImage) graphics.DrawImage(g_pBgImage, 0, 0, 420, 120);
        EndPaint(hwnd, &ps);
        break;
    }

    case WM_COMMAND:
        // Кнопка Обзор
        if (LOWORD(wParam) == 102) {
            BrowseFolder(hwnd);
        }

        // Кнопка INSTALL
        if (LOWORD(wParam) == 101) {
            // Читаем актуальный путь из Edit-поля
            wchar_t currentPath[MAX_PATH];
            GetWindowTextW(g_hEditPath, currentPath, MAX_PATH);
            std::wstring finalPath = currentPath;

            if (finalPath.empty()) {
                MessageBoxW(hwnd, L"Пожалуйста, выберите путь!", L"Ошибка", MB_ICONERROR);
                break;
            }

            EnableWindow((HWND)lParam, FALSE);
            SendMessage(g_hProgressBar, PBM_SETPOS, 0, 0);

            InstallParams* p = new InstallParams{ hwnd, finalPath };
            _beginthreadex(NULL, 0, InstallThread, p, 0, NULL);
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdiplusToken, &gsi, NULL);
    g_pBgImage = LoadImageFromResource(hInst, IDR_IMAGE_BG, L"IMAGE");

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_MAIN_ICON));
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"InstallerClass";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, L"InstallerClass", g_AppName.c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (GetSystemMetrics(SM_CXSCREEN) - 420) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - 330) / 2,
        420, 330, NULL, NULL, hInst, NULL);

    ShowWindow(hwnd, nCmdShow);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    delete g_pBgImage;
    GdiplusShutdown(g_gdiplusToken);
    return 0;
}