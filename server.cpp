#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <uxtheme.h>
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
#pragma comment(lib, "uxtheme.lib")
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")

const int PORT = 4444;
const int PANEL_W = 264;

// ── state ──
static std::vector<BYTE>  g_Frame;
static std::mutex         g_FrameLock;
static SOCKET             g_Client = INVALID_SOCKET;
static std::mutex         g_SockLock;

// ── ui ──
static HWND g_hWnd, g_hSlider, g_hListBox, g_hPlayBtn, g_hStatus;
static std::vector<std::string> g_Sounds;

// ── theme colours ──
static const COLORREF C_PANEL   = RGB(38, 38, 44);
static const COLORREF C_DISPLAY = RGB(24, 24, 28);
static const COLORREF C_LISTBG  = RGB(48, 48, 54);
static const COLORREF C_TEXT    = RGB(225, 225, 230);
static const COLORREF C_DIM     = RGB(110, 110, 120);
static const COLORREF C_LINE    = RGB(58, 58, 66);
static const COLORREF C_GREEN   = RGB(87, 206, 117);
static const COLORREF C_AMBER   = RGB(230, 160, 60);

static HFONT  s_fontTitle, s_fontLabel, s_fontSmall;
static HBRUSH s_brPanel, s_brList, s_brDisplay;

enum { ID_SLIDER = 101, ID_LISTBOX = 102, ID_PLAY = 103 };

// ──────────────────────── network ────────────────────────

bool RecvAll(SOCKET s, char* d, int len) {
    int t = 0;
    while (t < len) {
        int n = recv(s, d + t, len - t, 0);
        if (n <= 0) return false;
        t += n;
    }
    return true;
}

bool SendCmd(char cmd, const char* data = nullptr, int len = 0) {
    std::lock_guard<std::mutex> lk(g_SockLock);
    if (g_Client == INVALID_SOCKET) return false;
    if (send(g_Client, &cmd, 1, 0) != 1) return false;
    if (len > 0 && data && send(g_Client, data, len, 0) != len) return false;
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

void RefreshStatus() {
    bool ok;
    {
        std::lock_guard<std::mutex> lk(g_SockLock);
        ok = (g_Client != INVALID_SOCKET);
    }
    if (g_hStatus)
        SetWindowTextW(g_hStatus, ok ? L"\u25CF  Connected" : L"\u25CB  Waiting for client\u2026");
}

void FrameThread() {
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) return;

    BOOL reuse = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_port        = htons(PORT);
    a.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) { closesocket(srv); return; }
    if (listen(srv, 1) == SOCKET_ERROR)                       { closesocket(srv); return; }

    while (true) {
        SOCKET cli = accept(srv, NULL, NULL);
        if (cli == INVALID_SOCKET) continue;

        {
            std::lock_guard<std::mutex> lk(g_SockLock);
            g_Client = cli;
        }
        RefreshStatus();

        int len;
        while (RecvAll(cli, (char*)&len, 4) && len > 0 && len < 10 * 1024 * 1024) {
            std::vector<BYTE> buf(len);
            if (!RecvAll(cli, (char*)buf.data(), len)) break;
            {
                std::lock_guard<std::mutex> lk(g_FrameLock);
                g_Frame = std::move(buf);
            }
        }

        {
            std::lock_guard<std::mutex> lk(g_SockLock);
            g_Client = INVALID_SOCKET;
        }
        closesocket(cli);
        RefreshStatus();
    }
}

// ──────────────────────── ui helpers ────────────────────────

void PlaySelected() {
    int sel = (int)SendMessageA(g_hListBox, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || sel >= (int)g_Sounds.size()) return;
    std::string file = g_Sounds[sel];
    if (file.find('\\') == std::string::npos) {
        char dir[MAX_PATH];
        GetCurrentDirectoryA(MAX_PATH, dir);
        file = std::string(dir) + "\\" + file;
    }
    SendPlay(file);
}

void BuildControls(HWND hwnd) {
    s_fontTitle = CreateFontW(22, 0, 0, 0, FW_BOLD, 0, 0, 0,
                   DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    s_fontLabel = CreateFontW(13, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                   DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    s_fontSmall = CreateFontW(12, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                   DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    s_brPanel   = CreateSolidBrush(C_PANEL);
    s_brList    = CreateSolidBrush(C_LISTBG);
    s_brDisplay = CreateSolidBrush(C_DISPLAY);

    const int x = 18, w = PANEL_W - 36;

    // title
    HWND hT = CreateWindowExW(0, L"STATIC", L"NYX REMOTE",
              WS_CHILD | WS_VISIBLE | SS_LEFT,
              x, 14, w, 28, hwnd, 0, GetModuleHandle(0), 0);
    SendMessage(hT, WM_SETFONT, (WPARAM)s_fontTitle, 0);

    // "VOLUME" label
    HWND hV = CreateWindowExW(0, L"STATIC", L"VOLUME",
              WS_CHILD | WS_VISIBLE | SS_LEFT,
              x, 54, w, 16, hwnd, 0, GetModuleHandle(0), 0);
    SendMessage(hV, WM_SETFONT, (WPARAM)s_fontLabel, 0);

    // slider
    g_hSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTO_TICKS,
                x, 74, w, 30, hwnd, (HMENU)ID_SLIDER, GetModuleHandle(0), 0);
    SendMessage(g_hSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessage(g_hSlider, TBM_SETPOS,   TRUE, 50);
    SendMessage(g_hSlider, TBM_SETTICFREQ, 25, 0);

    // "SOUNDS" label
    HWND hS = CreateWindowExW(0, L"STATIC", L"SOUNDS",
              WS_CHILD | WS_VISIBLE | SS_LEFT,
              x, 114, w, 16, hwnd, 0, GetModuleHandle(0), 0);
    SendMessage(hS, WM_SETFONT, (WPARAM)s_fontLabel, 0);

    // listbox
    g_hListBox = CreateWindowExW(WS_EX_STATICEDGE, L"LISTBOX", L"",
                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY |
                 LBS_NOINTEGRALHEIGHT,
                 x, 136, w, 200, hwnd, (HMENU)ID_LISTBOX,
                 GetModuleHandle(0), 0);
    SendMessage(g_hListBox, WM_SETFONT, (WPARAM)s_fontSmall, 0);
    SetWindowTheme(g_hListBox, L"", L"");

    for (const auto& s : g_Sounds) {
        std::string name = s;
        auto p = name.find_last_of("\\/");
        if (p != std::string::npos) name = name.substr(p + 1);
        int idx = (int)SendMessageA(g_hListBox, LB_ADDSTRING, 0, (LPARAM)name.c_str());
        SendMessageA(g_hListBox, LB_SETITEMDATA, idx, (LPARAM)&g_Sounds[idx]);
    }
    if (!g_Sounds.empty())
        SendMessageA(g_hListBox, LB_SETCURSEL, 0, 0);

    // play button
    g_hPlayBtn = CreateWindowExW(0, L"BUTTON", L"\u25B6  Play Sound",
                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                 x, 346, w, 34, hwnd, (HMENU)ID_PLAY,
                 GetModuleHandle(0), 0);
    SendMessage(g_hPlayBtn, WM_SETFONT, (WPARAM)s_fontLabel, 0);
    SetWindowTheme(g_hPlayBtn, L"", L"");

    if (g_Sounds.empty())
        EnableWindow(g_hPlayBtn, FALSE);

    // status
    g_hStatus = CreateWindowExW(0, L"STATIC", L"\u25CB  Waiting for client\u2026",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                x, 394, w, 18, hwnd, 0, GetModuleHandle(0), 0);
    SendMessage(g_hStatus, WM_SETFONT, (WPARAM)s_fontSmall, 0);
}

// ──────────────────────── wndproc ────────────────────────

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE:
        BuildControls(hwnd);
        SetTimer(hwnd, 1, 80, NULL);
        return 0;

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT rc; GetClientRect(hwnd, &rc);
        RECT panel   = { 0, 0, PANEL_W, rc.bottom };
        RECT display = { PANEL_W, 0, rc.right, rc.bottom };
        FillRect(hdc, &panel,   s_brPanel);
        FillRect(hdc, &display, s_brDisplay);
        // divider
        HPEN pen = CreatePen(PS_SOLID, 1, C_LINE);
        HPEN old = (HPEN)SelectObject(hdc, pen);
        MoveToEx(hdc, PANEL_W, 0, NULL);
        LineTo(hdc, PANEL_W, rc.bottom);
        // separators
        MoveToEx(hdc, 18, 46, NULL);  LineTo(hdc, PANEL_W - 18, 46);
        MoveToEx(hdc, 18, 106, NULL); LineTo(hdc, PANEL_W - 18, 106);
        SelectObject(hdc, old);
        DeleteObject(pen);
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc  = (HDC)wp;
        HWND ctl = (HWND)lp;
        if (ctl == g_hStatus) {
            bool ok;
            { std::lock_guard<std::mutex> lk(g_SockLock); ok = (g_Client != INVALID_SOCKET); }
            SetTextColor(hdc, ok ? C_GREEN : C_AMBER);
        } else {
            SetTextColor(hdc, C_TEXT);
        }
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)s_brPanel;
    }

    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, C_TEXT);
        SetBkColor(hdc, C_LISTBG);
        return (LRESULT)s_brList;
    }

    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, C_TEXT);
        SetBkColor(hdc, C_PANEL);
        return (LRESULT)s_brPanel;
    }

    case WM_HSCROLL:
        if ((HWND)lp == g_hSlider) {
            int pos = (int)SendMessage(g_hSlider, TBM_GETPOS, 0, 0);
            SendVolume(pos);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_PLAY) {
            PlaySelected();
        } else if (LOWORD(wp) == ID_LISTBOX && HIWORD(wp) == LBN_DBLCLK) {
            PlaySelected();
        }
        return 0;

    case WM_TIMER:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        std::vector<BYTE> frame;
        {
            std::lock_guard<std::mutex> lk(g_FrameLock);
            frame = g_Frame;
        }

        if (!frame.empty()) {
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, frame.size());
            void* p = GlobalLock(hMem);
            memcpy(p, frame.data(), frame.size());
            GlobalUnlock(hMem);

            IStream* strm = 0;
            CreateStreamOnHGlobal(hMem, TRUE, &strm);
            Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromStream(strm);

            if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
                int bw = bmp->GetWidth(), bh = bmp->GetHeight();
                int pad = 10;
                int ax = PANEL_W + pad, ay = pad;
                int aw = rc.right - ax - pad, ah = rc.bottom - ay - pad;
                if (aw > 0 && ah > 0 && bw > 0 && bh > 0) {
                    float scale = min((float)aw / bw, (float)ah / bh);
                    int dw = (int)(bw * scale), dh = (int)(bh * scale);
                    int dx = ax + (aw - dw) / 2, dy = ay + (ah - dh) / 2;

                    // dark border around image
                    Gdiplus::Graphics g(hdc);
                    Gdiplus::SolidBrush br(Gdiplus::Color(20, 20, 24));
                    g.FillRectangle(&br, dx - 2, dy - 2, dw + 4, dh + 4);
                    g.DrawImage(bmp, dx, dy, dw, dh);
                }
            }
            delete bmp;
            strm->Release();
        } else {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, C_DIM);
            HFONT old = (HFONT)SelectObject(hdc, s_fontLabel);
            const char* txt = "Waiting for screen data...";
            int tx = PANEL_W + (rc.right - PANEL_W - 220) / 2;
            int ty = rc.bottom / 2 - 8;
            TextOutA(hdc, tx, ty, txt, (int)strlen(txt));
            SelectObject(hdc, old);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ──────────────────────── init ────────────────────────

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
    // ═══════ THIS WAS MISSING — every Winsock call was failing ═══════
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

    Gdiplus::GdiplusStartupInput gdip;
    ULONG_PTR gdipTok;
    Gdiplus::GdiplusStartup(&gdipTok, &gdip, 0);

    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icex);

    FindMP3s();
    std::thread(FrameThread).detach();

    WNDCLASSEXA wc = { sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = GetModuleHandle(0);
    wc.hCursor        = LoadCursor(0, IDC_ARROW);
    wc.lpszClassName  = "NyxServer";
    RegisterClassExA(&wc);

    g_hWnd = CreateWindowExA(0, "NyxServer", "Nyx Remote",
                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, 1040, 660,
                0, 0, wc.hInstance, 0);
    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    MSG msg;
    while (GetMessage(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Gdiplus::GdiplusShutdown(gdipTok);
    WSACleanup();
    return 0;
}