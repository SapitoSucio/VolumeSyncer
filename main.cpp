#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <algorithm>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <string>
#include <combaseapi.h>
#include <thread>
#include <chrono>
#include <functional>
#include <memory>
#include <shellapi.h>
#include <shlobj.h>
#include <cwchar>
#include <fstream>

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Uuid.lib")
#pragma comment(lib, "Shell32.lib")

#define IDM_EXIT 101
#define IDM_TOGGLE_NOTIFICATIONS 102
#define IDM_TOGGLE_AUTOSTART 103

// Estructura para datos del icono de la bandeja
struct TrayIconData {
    NOTIFYICONDATA nid;
    HICON hIcon;
    HWND hWnd;
};

// Datos globales del icono de la bandeja
TrayIconData trayData;

bool show_notifications = true;
bool auto_start = false;

// Mutex para prevenir múltiples instancias
HANDLE hMutex = NULL;

// Función para mostrar notificaciones
void show_notification(const std::wstring& title, const std::wstring& message) {
    trayData.nid.uFlags = NIF_INFO;
    wcsncpy_s(trayData.nid.szInfoTitle, title.c_str(), ARRAYSIZE(trayData.nid.szInfoTitle) - 1);
    wcsncpy_s(trayData.nid.szInfo, message.c_str(), ARRAYSIZE(trayData.nid.szInfo) - 1);
    trayData.nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIcon(NIM_MODIFY, &trayData.nid);
}

// Inicializar el icono de la bandeja
void init_tray_icon(HWND hWnd) {
    trayData.hWnd = hWnd;
    trayData.hIcon = LoadIcon(NULL, IDI_APPLICATION);

    trayData.nid.cbSize = sizeof(NOTIFYICONDATA);
    trayData.nid.hWnd = hWnd;
    trayData.nid.uID = 1;
    trayData.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    trayData.nid.uCallbackMessage = WM_APP + 1;
    trayData.nid.hIcon = trayData.hIcon;
    wcscpy_s(trayData.nid.szTip, L"VolumeSyncer");

    if (!Shell_NotifyIcon(NIM_ADD, &trayData.nid)) {
        std::wcerr << L"Error al agregar el icono a la bandeja del sistema." << std::endl;
    }
}

// Limpiar el icono de la bandeja
void cleanup_tray_icon() {
    Shell_NotifyIcon(NIM_DELETE, &trayData.nid);
    if (trayData.hIcon) {
        DestroyIcon(trayData.hIcon);
    }
}

// Obtener el dispositivo de audio predeterminado
std::shared_ptr<IAudioEndpointVolume> get_default_device() {
    HRESULT hr = CoInitialize(nullptr);
    if (FAILED(hr)) {
        std::wcerr << L"CoInitialize falló: " << std::hex << hr << std::endl;
        return nullptr;
    }

    IMMDeviceEnumerator* deviceEnumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator), (void**)&deviceEnumerator);
    if (FAILED(hr)) {
        std::wcerr << L"CoCreateInstance falló: " << std::hex << hr << std::endl;
        CoUninitialize();
        return nullptr;
    }

    IMMDevice* defaultDevice = nullptr;
    hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice);
    if (FAILED(hr)) {
        std::wcerr << L"GetDefaultAudioEndpoint falló: " << std::hex << hr << std::endl;
        deviceEnumerator->Release();
        CoUninitialize();
        return nullptr;
    }

    IAudioEndpointVolume* endpointVolume = nullptr;
    hr = defaultDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&endpointVolume);
    if (FAILED(hr)) {
        std::wcerr << L"Activate falló: " << std::hex << hr << std::endl;
        defaultDevice->Release();
        deviceEnumerator->Release();
        CoUninitialize();
        return nullptr;
    }

    deviceEnumerator->Release();
    defaultDevice->Release();

    return std::shared_ptr<IAudioEndpointVolume>(endpointVolume, [](IAudioEndpointVolume* p) {
        if (p) {
            p->Release();
        }
        CoUninitialize();
        });
}

bool check_and_balance_audio() {
    auto device = get_default_device();
    if (!device) return false;

    float left = 0.0f, right = 0.0f;
    device->GetChannelVolumeLevelScalar(0, &left);
    device->GetChannelVolumeLevelScalar(1, &right);

    if (std::abs(left - right) > 0.01f) {
        float max_volume = std::max(left, right);
        try {
            device->SetChannelVolumeLevelScalar(0, max_volume, nullptr);
            device->SetChannelVolumeLevelScalar(1, max_volume, nullptr);

            if (show_notifications) {
                show_notification(L"VolumeSyncer", L"Canales de audio equilibrados a " + std::to_wstring(max_volume));
            }
            return true;
        }
        catch (...) {
            std::cerr << "Error al equilibrar el audio" << std::endl;
        }
    }
    return false;
}

// Funciones para guardar y cargar configuraiones
void save_settings() {
    std::ofstream file("volume_syncer_settings.txt");
    if (file.is_open()) {
        file << show_notifications << "\n";
        file.close();
    }
}

void load_settings() {
    std::ifstream file("volume_syncer_settings.txt");
    if (file.is_open()) {
        file >> show_notifications;
        file.close();
    }
}

bool is_auto_start_enabled() {
    HKEY hKey;
    LONG result = RegOpenKeyEx(HKEY_CURRENT_USER, LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)", 0, KEY_READ, &hKey);
    if (result == ERROR_SUCCESS) {
        DWORD type;
        DWORD dataSize = 0;
        result = RegQueryValueEx(hKey, L"VolumeSyncer", NULL, &type, NULL, &dataSize);
        RegCloseKey(hKey);
        return (result == ERROR_SUCCESS);
    }
    return false;
}

void add_to_startup() {
    HKEY hKey;
    LONG result = RegOpenKeyEx(HKEY_CURRENT_USER, LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)", 0, KEY_SET_VALUE, &hKey);
    if (result == ERROR_SUCCESS) {
        wchar_t path[MAX_PATH];
        if (GetModuleFileNameW(NULL, path, MAX_PATH) != 0) {
            result = RegSetValueExW(hKey, L"VolumeSyncer", 0, REG_SZ,
                reinterpret_cast<BYTE*>(path),
                (wcslen(path) + 1) * sizeof(wchar_t));
            if (result != ERROR_SUCCESS) {
                std::wcerr << L"Error al establecer el valor del registro: " << result << std::endl;
            }
        }
        else {
            std::wcerr << L"Error al obtener la ruta del ejecutable" << std::endl;
        }
        RegCloseKey(hKey);
    }
    else {
        std::wcerr << L"Error al abrir la clave del registro: " << result << std::endl;
    }
}

void remove_from_startup() {
    HKEY hKey;
    LONG result = RegOpenKeyEx(HKEY_CURRENT_USER, LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)", 0, KEY_ALL_ACCESS, &hKey);
    if (result == ERROR_SUCCESS) {
        RegDeleteValue(hKey, L"VolumeSyncer");
        RegCloseKey(hKey);
    }
}

void toggle_notifications() {
    show_notifications = !show_notifications;
    save_settings();
    show_notification(L"VolumeSyncer", show_notifications ? L"Notificaciones activadas" : L"Notificaciones desactivadas");
}

void toggle_auto_start() {
    auto_start = !auto_start;
    if (auto_start) {
        add_to_startup();
    }
    else {
        remove_from_startup();
    }
}

void run_volume_syncer() {
    while (true) {
        check_and_balance_audio();
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_APP + 1) {
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            if (hMenu) {
                UINT flags = MF_BYPOSITION | MF_STRING;
                InsertMenu(hMenu, -1, flags | (show_notifications ? MF_CHECKED : MF_UNCHECKED),
                    IDM_TOGGLE_NOTIFICATIONS, show_notifications ? L"Notificaciones Activadas" : L"Notificaciones Desactivadas");

                auto_start = is_auto_start_enabled();  // Actualizar el estado de auto_start
                InsertMenu(hMenu, -1, flags | (auto_start ? MF_CHECKED : MF_UNCHECKED),
                    IDM_TOGGLE_AUTOSTART, auto_start ? L"Inicio Automático Activado" : L"Inicio Automático Desactivado");

                InsertMenu(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
                InsertMenu(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_EXIT, L"Salir");

                SetForegroundWindow(hwnd);
                TrackPopupMenu(hMenu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
            }

        }
    }
    else if (uMsg == WM_COMMAND) {
        switch (LOWORD(wParam)) {
        case IDM_EXIT:
            PostQuitMessage(0);
            break;
        case IDM_TOGGLE_NOTIFICATIONS:
            toggle_notifications();
            break;
        case IDM_TOGGLE_AUTOSTART:
            toggle_auto_start();
            break;
        }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Crear un mutex para prevenir múltiples instancias
    hMutex = CreateMutex(NULL, TRUE, L"Global\\VolumeSyncerMutex");
    if (hMutex == NULL) {
        MessageBox(NULL, L"Error al crear el mutex", L"VolumeSyncer", MB_ICONERROR);
        return 1;
    }

    // Comprobar si ya existe una instancia en ejecución
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBox(NULL, L"VolumeSyncer ya está en ejecución", L"VolumeSyncer", MB_ICONINFORMATION);
        CloseHandle(hMutex);
        return 0;
    }

    load_settings();
    auto_start = is_auto_start_enabled();
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"VolumeSyncerTrayClass";
    RegisterClass(&wc);

    HWND hWnd = CreateWindow(wc.lpszClassName, L"VolumeSyncer", 0, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
    init_tray_icon(hWnd);
    std::thread audio_thread(run_volume_syncer);
    audio_thread.detach();

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    cleanup_tray_icon();
    CoUninitialize();

    // Liberar el mutex al salir
    if (hMutex) {
        CloseHandle(hMutex);
    }

    return 0;
}