#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <gdiplus.h>
#include <commctrl.h>
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

std::vector<BYTE> g_Frame;
std::mutex g_FrameLock;
SOCKET g_Client = INVALID_SOCKET;
std::mutex g_SockLock;

HWND g_hWnd, g_hSlider;
std::vector<std::string> g_Sounds;
std::vector<HWND> g_Buttons;

// ---------- network helpers ----------
bool RecvAll(SOCKET s, char* d, int len) {
    int total = 0;
    while (total < len) {
        int n = recv(s, d + total, len - total, 0);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

bool SendCmd(char cmd, const char* data = nullptr, int len = 0) {
    std::lock_guard<std::mutex> lk(g_SockLock);
    if (g_Client == INVALID_SOCKET) return false;
    if (send(g_Client, &cmd, 1, 0) != 1) return false;
    if (len && data && send(g_Client, data, len, 0) != len) return false;
    return true;
}

bool SendVolume(int vol) {
    return SendCmd('V', (char*)&vol, sizeof(vol));
}

bool SendPlay(const std::string& file) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return false;
    int sz = (int)in.tellg();
    in.seekg(0);
    std::vector<char> buf(sz);
    in.read(buf.data(), sz);
    std::lock_guard<std::mutex> lk(g_SockLock);
    if (g_Client == INVALID_SOCKET) return false;
    if (send(g_Client, "P", 1, 0) != 1) return false;
    if (send(g_Client, (char*)&sz, sizeof(sz), 0) != sizeof(sz)) return false;
    if (send(g_Client, buf.data(), sz, 0) != sz) return false;
    return true;
}

// ---------- frame receiver ----------
void FrameThread() {
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(PORT);
    a.sin_addr.s_addr = INADDR_ANY;
    bind(srv, (sockaddr*)&a, sizeof(a));
    listen(srv, 1);

    while (true) {
        SOCKET cli = accept(srv, NULL, NULL);
        if (cli == INVALID_SOCKET) continue;
        {
            std::lock_guard<std::mutex> lk(g_SockLock);
            g_Client = cli;
        }

        int len;
        // FIX: use RecvAll instead of single recv() for the 4-byte length
        while (RecvAll(cli, (char*)&len, 4) && len > 0 && len < 10*1024*1024) {
            std::vector<BYTE> buf(len);
            // FIX: use RecvAll for frame data too (simpler + correct)
            if (!RecvAll(cli, (char*)buf.data(), len)) break;
            {
                std::lock_guard<std::mutex> lk(g_FrameLock);
                g_Frame = std::move(buf);
            }
        }
        // FIX: invalidate before close so SendCmd can't use a closed socket
        {
            std::lock_guard<std::mutex> lk(g_SockLock);
            g_Client = INVALID_SOCKET;
        }
        closesocket(cli);
    }
}

// ---------- window procedure ----------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        g_hSlider = CreateWindowExA(0, TRACKBAR_CLASSA, "", WS_CHILD|WS_VISIBLE|TBS_HORZ|TBS_NOTICKS,
                                    10, 10, 200, 30, hwnd, (HMENU)101, GetModuleHandle(0), 0);
        SendMessage(g_hSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendMessage(g_hSlider, TBM_SETPOS, TRUE, 50);

        int y = 50;
        for (size_t i = 0; i < g_Sounds.size(); ++i, y += 30) {
            HWND btn = CreateWindowExA(0, "BUTTON", g_Sounds[i].c_str(),
                        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 10, y, 200, 25,
                        hwnd, (HMENU)(200 + i), GetModuleHandle(0), 0);
            g_Buttons.push_back(btn);
        }
        SetTimer(hwnd, 1, 200, NULL);
        return 0;
    }
    case WM_HSCROLL:
        if ((HWND)lp == g_hSlider) {
            int pos = (int)SendMessage(g_hSlider, TBM_GETPOS, 0, 0);
            SendVolume(pos);
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= 200 && id < 200 + (int)g_Sounds.size()) {
            std::string file = g_Sounds[id - 200];
            if (file.find('\\') == std::string::npos) {
                char curDir[MAX_PATH];
                GetCurrentDirectoryA(MAX_PATH, curDir);
                file = std::string(curDir) + "\\" + file;
            }
            SendPlay(file);
        }
        return 0;
    }
    case WM_TIMER:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)(COLOR_WINDOW+1));
        std::vector<BYTE> frame;
        {
            std::lock_guard<std::mutex> lk(g_FrameLock);
            frame = g_Frame;
        }
        if (!frame.empty()) {
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, frame.size());
            void* pMem = GlobalLock(hMem);
            memcpy(pMem, frame.data(), frame.size());
            GlobalUnlock(hMem);
            IStream* pStream = 0;
            CreateStreamOnHGlobal(hMem, TRUE, &pStream);
            Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromStream(pStream);
            if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
                int bw = bmp->GetWidth(), bh = bmp->GetHeight();
                int ww = rc.right-rc.left, wh = rc.bottom-rc.top;
                int offY = 120, availH = wh - offY;
                if (availH <= 0) availH = 1;
                float scale = min((float)ww/bw, (float)availH/bh);
                int dw = (int)(bw*scale), dh = (int)(bh*scale);
                int dx = (ww-dw)/2, dy = offY + (availH-dh)/2;
                Gdiplus::Graphics g(hdc);
                g.DrawImage(bmp, dx, dy, dw, dh);
            }
            delete bmp;
            pStream->Release();
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

void FindMP3s() {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("*.mp3", &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        g_Sounds.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

int main() {
    Gdiplus::GdiplusStartupInput gdip;
    ULONG_PTR gdipToken;
    Gdiplus::GdiplusStartup(&gdipToken, &gdip, 0);

    INITCOMMONCONTROLSEX icex = {sizeof(icex), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icex);

    FindMP3s();

    std::thread(FrameThread).detach();

    WNDCLASSEXA wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(0);
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName = "NyxServer";
    RegisterClassExA(&wc);

    g_hWnd = CreateWindowExA(0, "NyxServer", "Nyx Remote",
                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,0, 800,600,
                0,0,wc.hInstance,0);
    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    MSG msg;
    while (GetMessage(&msg,0,0,0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Gdiplus::GdiplusShutdown(gdipToken);
    return 0;
}