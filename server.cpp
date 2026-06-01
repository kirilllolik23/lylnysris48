#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
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

const int PORT        = 4444;
const int PORT_AUDIO  = 4445;
const int PORT_DEVS   = 4446;
const int PANEL_W     = 264;

#ifndef WAVE_FORMAT_PCM
#define WAVE_FORMAT_PCM 1
#endif

// ── state ──
static std::vector<BYTE>  g_Frame;
static std::mutex         g_FrameLock;
static SOCKET             g_Client = INVALID_SOCKET;
static std::mutex         g_SockLock;

// ── ui ──
static HWND g_hWnd, g_hSlider, g_hListBox, g_hPlayBtn, g_hStopBtn, g_hStatus;
static HWND g_hPcAudioChk, g_hMicChk, g_hMicCombo;
static std::vector<std::string> g_Sounds;
static std::vector<std::string> g_MicDevs;
static bool g_PcAudioOn = false, g_MicOn = false;

// ── colours ──
static const COLORREF C_PANEL   = RGB(38,38,44);
static const COLORREF C_DISPLAY = RGB(24,24,28);
static const COLORREF C_LISTBG  = RGB(48,48,54);
static const COLORREF C_TEXT    = RGB(225,225,230);
static const COLORREF C_DIM    = RGB(110,110,120);
static const COLORREF C_LINE   = RGB(58,58,66);
static const COLORREF C_GREEN  = RGB(87,206,117);
static const COLORREF C_AMBER  = RGB(230,160,60);
static const COLORREF C_RED    = RGB(220,80,80);

static HFONT  s_fontTitle, s_fontLabel, s_fontSmall;
static HBRUSH s_brPanel, s_brList, s_brDisplay;

enum { ID_SLIDER=101, ID_LISTBOX=102, ID_PLAY=103, ID_STOP=107,
       ID_PC_AUDIO=104, ID_MIC=105, ID_MIC_COMBO=106,
       WM_DEVLIST = WM_USER+1 };

// ──────────────────────── network ────────────────────────

bool RecvAll(SOCKET s, char* d, int len) {
    int t=0;
    while(t<len){ int n=recv(s,d+t,len-t,0); if(n<=0) return false; t+=n; }
    return true;
}

bool SendCmd(char cmd, const char* data=nullptr, int len=0) {
    std::lock_guard<std::mutex> lk(g_SockLock);
    if(g_Client==INVALID_SOCKET) return false;
    if(send(g_Client,&cmd,1,0)!=1) return false;
    if(len>0 && data && send(g_Client,data,len,0)!=len) return false;
    return true;
}

bool SendVolume(int vol) { return SendCmd('V',(char*)&vol,sizeof(vol)); }
bool SendPcAudio(bool on) { char d=on?1:0; return SendCmd('A',&d,1); }
bool SendStop() { return SendCmd('S'); }

bool SendMic(bool on, int devIdx) {
    std::lock_guard<std::mutex> lk(g_SockLock);
    if(g_Client==INVALID_SOCKET) return false;
    char d=on?1:0;
    if(send(g_Client,"M",1,0)!=1) return false;
    if(send(g_Client,&d,1,0)!=1) return false;
    if(on && send(g_Client,(char*)&devIdx,4,0)!=4) return false;
    return true;
}

bool SendPlay(const std::string& file) {
    std::ifstream in(file, std::ios::binary|std::ios::ate);
    if(!in) return false;
    int sz=(int)in.tellg(); in.seekg(0);
    std::vector<char> buf(sz); in.read(buf.data(),sz);
    std::lock_guard<std::mutex> lk(g_SockLock);
    if(g_Client==INVALID_SOCKET) return false;
    if(send(g_Client,"P",1,0)!=1) return false;
    if(send(g_Client,(char*)&sz,sizeof(sz),0)!=sizeof(sz)) return false;
    if(send(g_Client,buf.data(),sz,0)!=sz) return false;
    return true;
}

void RefreshStatus() {
    bool ok;
    { std::lock_guard<std::mutex> lk(g_SockLock); ok=(g_Client!=INVALID_SOCKET); }
    if(g_hStatus)
        SetWindowTextW(g_hStatus, ok?L"\u25CF  Connected":L"\u25CB  Waiting for client\u2026");
    if(ok) {
        SendCmd('L');
        g_PcAudioOn=false; g_MicOn=false;
        if(g_hPcAudioChk) SendMessage(g_hPcAudioChk,BM_SETCHECK,BST_UNCHECKED,0);
        if(g_hMicChk)     SendMessage(g_hMicChk,BM_SETCHECK,BST_UNCHECKED,0);
    }
}

// ──────────────────────── frame receiver ────────────────────────

void FrameThread() {
    SOCKET srv=socket(AF_INET,SOCK_STREAM,0);
    BOOL reuse=TRUE; setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,(char*)&reuse,sizeof(reuse));
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(PORT); a.sin_addr.s_addr=INADDR_ANY;
    if(bind(srv,(sockaddr*)&a,sizeof(a))==SOCKET_ERROR){closesocket(srv);return;}
    if(listen(srv,1)==SOCKET_ERROR){closesocket(srv);return;}
    while(true){
        SOCKET cli=accept(srv,NULL,NULL);
        if(cli==INVALID_SOCKET) continue;
        { std::lock_guard<std::mutex> lk(g_SockLock); g_Client=cli; }
        RefreshStatus();
        int len;
        while(RecvAll(cli,(char*)&len,4) && len>0 && len<10*1024*1024){
            std::vector<BYTE> buf(len);
            if(!RecvAll(cli,(char*)buf.data(),len)) break;
            { std::lock_guard<std::mutex> lk(g_FrameLock); g_Frame=std::move(buf); }
        }
        { std::lock_guard<std::mutex> lk(g_SockLock); g_Client=INVALID_SOCKET; }
        closesocket(cli);
        RefreshStatus();
    }
}

// ──────────────────────── audio receiver ────────────────────────

void AudioReceiverThread() {
    SOCKET srv=socket(AF_INET,SOCK_STREAM,0);
    BOOL reuse=TRUE; setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,(char*)&reuse,sizeof(reuse));
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(PORT_AUDIO); a.sin_addr.s_addr=INADDR_ANY;
    if(bind(srv,(sockaddr*)&a,sizeof(a))==SOCKET_ERROR){closesocket(srv);return;}
    if(listen(srv,1)==SOCKET_ERROR){closesocket(srv);return;}

    while(true){
        SOCKET cli=accept(srv,NULL,NULL);
        if(cli==INVALID_SOCKET) continue;

        int sampleRate; short channels;
        if(!RecvAll(cli,(char*)&sampleRate,4)||!RecvAll(cli,(char*)&channels,2)){
            closesocket(cli); continue;
        }

        CoInitialize(0);
        WAVEFORMATEX wfx{}; 
        wfx.wFormatTag=WAVE_FORMAT_PCM; wfx.nChannels=channels;
        wfx.nSamplesPerSec=sampleRate; wfx.wBitsPerSample=16;
        wfx.nBlockAlign=channels*2; wfx.nAvgBytesPerSec=sampleRate*wfx.nBlockAlign;

        IMMDeviceEnumerator* pEnum=NULL; IMMDevice* pDev=NULL;
        IAudioClient* pAC=NULL; IAudioRenderClient* pRC=NULL;
        HRESULT hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),0,CLSCTX_ALL,
                     __uuidof(IMMDeviceEnumerator),(void**)&pEnum);
        if(SUCCEEDED(hr)) hr=pEnum->GetDefaultAudioEndpoint(eRender,eConsole,&pDev);
        if(SUCCEEDED(hr)) hr=pDev->Activate(__uuidof(IAudioClient),CLSCTX_ALL,0,(void**)&pAC);
        if(SUCCEEDED(hr)) hr=pAC->Initialize(AUDCLNT_SHAREMODE_SHARED,0,10000000,0,&wfx,NULL);
        if(SUCCEEDED(hr)) hr=pAC->GetService(__uuidof(IAudioRenderClient),(void**)&pRC);

        if(SUCCEEDED(hr)){
            UINT32 bufFrames; pAC->GetBufferSize(&bufFrames); pAC->Start();
            while(true){
                int len;
                if(!RecvAll(cli,(char*)&len,4)||len<=0||len>1024*1024) break;
                std::vector<char> data(len);
                if(!RecvAll(cli,data.data(),len)) break;
                UINT32 frames=len/wfx.nBlockAlign;
                for(int w=0;w<50;w++){
                    UINT32 pad; pAC->GetCurrentPadding(&pad);
                    if(bufFrames-pad>=frames) break;
                    Sleep(2);
                }
                UINT32 pad; pAC->GetCurrentPadding(&pad);
                UINT32 avail=bufFrames-pad;
                if(frames>avail) frames=avail;
                if(!frames) continue;
                BYTE* pOut;
                if(SUCCEEDED(pRC->GetBuffer(frames,&pOut))){
                    memcpy(pOut,data.data(),frames*wfx.nBlockAlign);
                    pRC->ReleaseBuffer(frames,0);
                }
            }
            pAC->Stop();
        }
        if(pRC) pRC->Release(); if(pAC) pAC->Release();
        if(pDev) pDev->Release(); if(pEnum) pEnum->Release();
        CoUninitialize();
        closesocket(cli);
    }
}

// ──────────────────────── device list receiver ────────────────────────

void DevListReceiverThread() {
    SOCKET srv=socket(AF_INET,SOCK_STREAM,0);
    BOOL reuse=TRUE; setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,(char*)&reuse,sizeof(reuse));
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(PORT_DEVS); a.sin_addr.s_addr=INADDR_ANY;
    if(bind(srv,(sockaddr*)&a,sizeof(a))==SOCKET_ERROR){closesocket(srv);return;}
    if(listen(srv,1)==SOCKET_ERROR){closesocket(srv);return;}

    while(true){
        SOCKET cli=accept(srv,NULL,NULL);
        if(cli==INVALID_SOCKET) continue;
        int count;
        if(!RecvAll(cli,(char*)&count,4)){closesocket(cli);continue;}
        std::vector<std::string> devs;
        for(int i=0;i<count&&i<32;i++){
            int nl;
            if(!RecvAll(cli,(char*)&nl,4)||nl<0||nl>512) break;
            if(nl==0) continue;  // skip empty names instead of breaking
            std::vector<char> nm(nl+1); if(!RecvAll(cli,nm.data(),nl)) break;
            nm[nl]=0; devs.push_back(std::string(nm.data()));
        }
        g_MicDevs=devs;
        PostMessage(g_hWnd,WM_DEVLIST,0,0);
        closesocket(cli);
    }
}

// ──────────────────────── ui ────────────────────────

void PlaySelected(){
    int sel=(int)SendMessageA(g_hListBox,LB_GETCURSEL,0,0);
    if(sel==LB_ERR||sel>=(int)g_Sounds.size()) return;
    std::string file=g_Sounds[sel];
    if(file.find('\\')==std::string::npos){
        char dir[MAX_PATH]; GetCurrentDirectoryA(MAX_PATH,dir);
        file=std::string(dir)+"\\"+file;
    }
    SendPlay(file);
}

void BuildControls(HWND hwnd){
    s_fontTitle=CreateFontW(22,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    s_fontLabel=CreateFontW(13,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    s_fontSmall=CreateFontW(12,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    s_brPanel=CreateSolidBrush(C_PANEL); s_brList=CreateSolidBrush(C_LISTBG); s_brDisplay=CreateSolidBrush(C_DISPLAY);

    const int x=18, w=PANEL_W-36;
    int y=14;

    // title
    HWND hT=CreateWindowExW(0,L"STATIC",L"NYX REMOTE",WS_CHILD|WS_VISIBLE|SS_LEFT,x,y,w,28,hwnd,0,GetModuleHandle(0),0);
    SendMessage(hT,WM_SETFONT,(WPARAM)s_fontTitle,0); y+=46;

    // volume
    HWND hV=CreateWindowExW(0,L"STATIC",L"VOLUME",WS_CHILD|WS_VISIBLE|SS_LEFT,x,y,w,16,hwnd,0,GetModuleHandle(0),0);
    SendMessage(hV,WM_SETFONT,(WPARAM)s_fontLabel,0); y+=20;
    g_hSlider=CreateWindowExW(0,TRACKBAR_CLASSW,L"",WS_CHILD|WS_VISIBLE|TBS_HORZ|TBS_AUTOTICKS,
        x,y,w,30,hwnd,(HMENU)ID_SLIDER,GetModuleHandle(0),0);
    SendMessage(g_hSlider,TBM_SETRANGE,TRUE,MAKELONG(0,100));
    SendMessage(g_hSlider,TBM_SETPOS,TRUE,50);
    SendMessage(g_hSlider,TBM_SETTICFREQ,25,0); y+=40;

    // sounds
    HWND hS=CreateWindowExW(0,L"STATIC",L"SOUNDS",WS_CHILD|WS_VISIBLE|SS_LEFT,x,y,w,16,hwnd,0,GetModuleHandle(0),0);
    SendMessage(hS,WM_SETFONT,(WPARAM)s_fontLabel,0); y+=20;
    g_hListBox=CreateWindowExW(WS_EX_STATICEDGE,L"LISTBOX",L"",
        WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
        x,y,w,140,hwnd,(HMENU)ID_LISTBOX,GetModuleHandle(0),0);
    SendMessage(g_hListBox,WM_SETFONT,(WPARAM)s_fontSmall,0);
    SetWindowTheme(g_hListBox,L"",L"");
    for(const auto& s:g_Sounds){
        std::string name=s; auto p=name.find_last_of("\\/");
        if(p!=std::string::npos) name=name.substr(p+1);
        SendMessageA(g_hListBox,LB_ADDSTRING,0,(LPARAM)name.c_str());
    }
    if(!g_Sounds.empty()) SendMessageA(g_hListBox,LB_SETCURSEL,0,0);
    y+=148;

    // play + stop row
    int halfW = (w - 6) / 2;
    g_hPlayBtn=CreateWindowExW(0,L"BUTTON",L"\u25B6  Play",
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,x,y,halfW,30,hwnd,(HMENU)ID_PLAY,GetModuleHandle(0),0);
    SendMessage(g_hPlayBtn,WM_SETFONT,(WPARAM)s_fontLabel,0);
    SetWindowTheme(g_hPlayBtn,L"",L"");

    g_hStopBtn=CreateWindowExW(0,L"BUTTON",L"\u25A0  Stop",
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,x+halfW+6,y,halfW,30,hwnd,(HMENU)ID_STOP,GetModuleHandle(0),0);
    SendMessage(g_hStopBtn,WM_SETFONT,(WPARAM)s_fontLabel,0);
    SetWindowTheme(g_hStopBtn,L"",L"");

    if(g_Sounds.empty()) EnableWindow(g_hPlayBtn,FALSE);
    y+=40;

    // pc audio
    HWND hLA=CreateWindowExW(0,L"STATIC",L"PC AUDIO",WS_CHILD|WS_VISIBLE|SS_LEFT,x,y+3,80,16,hwnd,0,GetModuleHandle(0),0);
    SendMessage(hLA,WM_SETFONT,(WPARAM)s_fontLabel,0);
    g_hPcAudioChk=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
        x+w-20,y+2,20,20,hwnd,(HMENU)ID_PC_AUDIO,GetModuleHandle(0),0);
    SetWindowTheme(g_hPcAudioChk,L"",L"");
    y+=28;

    // microphone
    HWND hLM=CreateWindowExW(0,L"STATIC",L"MICROPHONE",WS_CHILD|WS_VISIBLE|SS_LEFT,x,y+3,100,16,hwnd,0,GetModuleHandle(0),0);
    SendMessage(hLM,WM_SETFONT,(WPARAM)s_fontLabel,0);
    g_hMicChk=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
        x+w-20,y+2,20,20,hwnd,(HMENU)ID_MIC,GetModuleHandle(0),0);
    SetWindowTheme(g_hMicChk,L"",L"");
    y+=28;

    // mic device combo
    g_hMicCombo=CreateWindowExW(0,L"COMBOBOX",L"",
        WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
        x,y,w,200,hwnd,(HMENU)ID_MIC_COMBO,GetModuleHandle(0),0);
    SendMessage(g_hMicCombo,WM_SETFONT,(WPARAM)s_fontSmall,0);
    SetWindowTheme(g_hMicCombo,L"",L"");
    y+=36;

    // status
    g_hStatus=CreateWindowExW(0,L"STATIC",L"\u25CB  Waiting for client\u2026",
        WS_CHILD|WS_VISIBLE|SS_LEFT,x,y,w,18,hwnd,0,GetModuleHandle(0),0);
    SendMessage(g_hStatus,WM_SETFONT,(WPARAM)s_fontSmall,0);
}

// ──────────────────────── wndproc ────────────────────────

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    switch(msg){
    case WM_CREATE:
        BuildControls(hwnd);
        SetTimer(hwnd,1,80,NULL);
        return 0;
    case WM_ERASEBKGND:{
        HDC hdc=(HDC)wp; RECT rc; GetClientRect(hwnd,&rc);
        RECT panel={0,0,PANEL_W,rc.bottom}, display={PANEL_W,0,rc.right,rc.bottom};
        FillRect(hdc,&panel,s_brPanel); FillRect(hdc,&display,s_brDisplay);
        HPEN pen=CreatePen(PS_SOLID,1,C_LINE), old=(HPEN)SelectObject(hdc,pen);
        MoveToEx(hdc,PANEL_W,0,NULL); LineTo(hdc,PANEL_W,rc.bottom);
        SelectObject(hdc,old); DeleteObject(pen);
        return 1;
    }
    case WM_CTLCOLORSTATIC:{
        HDC hdc=(HDC)wp; HWND ctl=(HWND)lp;
        if(ctl==g_hStatus){
            bool ok; {std::lock_guard<std::mutex> lk(g_SockLock); ok=(g_Client!=INVALID_SOCKET);}
            SetTextColor(hdc,ok?C_GREEN:C_AMBER);
        } else SetTextColor(hdc,C_TEXT);
        SetBkMode(hdc,TRANSPARENT); return (LRESULT)s_brPanel;
    }
    case WM_CTLCOLORLISTBOX:{
        HDC hdc=(HDC)wp; SetTextColor(hdc,C_TEXT); SetBkColor(hdc,C_LISTBG);
        return (LRESULT)s_brList;
    }
    case WM_CTLCOLORBTN:{
        HDC hdc=(HDC)wp; SetTextColor(hdc,C_TEXT); SetBkColor(hdc,C_PANEL);
        return (LRESULT)s_brPanel;
    }
    case WM_HSCROLL:
        if((HWND)lp==g_hSlider){ int pos=(int)SendMessage(g_hSlider,TBM_GETPOS,0,0); SendVolume(pos); }
        return 0;
    case WM_COMMAND:{
        int id=LOWORD(wp);
        if(id==ID_PLAY){ PlaySelected(); }
        else if(id==ID_STOP){ SendStop(); }
        else if(id==ID_LISTBOX && HIWORD(wp)==LBN_DBLCLK){ PlaySelected(); }
        else if(id==ID_PC_AUDIO){
            bool on=(SendMessage(g_hPcAudioChk,BM_GETCHECK,0,0)==BST_CHECKED);
            if(on){
                SendMessage(g_hMicChk,BM_SETCHECK,BST_UNCHECKED,0);
                g_MicOn=false; SendMic(false,0);
                g_PcAudioOn=true; SendPcAudio(true);
            } else { g_PcAudioOn=false; SendPcAudio(false); }
        }
        else if(id==ID_MIC){
            bool on=(SendMessage(g_hMicChk,BM_GETCHECK,0,0)==BST_CHECKED);
            if(on){
                SendMessage(g_hPcAudioChk,BM_SETCHECK,BST_UNCHECKED,0);
                g_PcAudioOn=false; SendPcAudio(false);
                int di=(int)SendMessage(g_hMicCombo,CB_GETCURSEL,0,0);
                if(di==CB_ERR) di=0;
                g_MicOn=true; SendMic(true,di);
            } else { g_MicOn=false; SendMic(false,0); }
        }
        return 0;
    }
    case WM_DEVLIST:
        SendMessageA(g_hMicCombo,CB_RESETCONTENT,0,0);
        for(const auto& d:g_MicDevs)
            SendMessageA(g_hMicCombo,CB_ADDSTRING,0,(LPARAM)d.c_str());
        if(!g_MicDevs.empty()) SendMessageA(g_hMicCombo,CB_SETCURSEL,0,0);
        EnableWindow(g_hMicChk,!g_MicDevs.empty());
        EnableWindow(g_hMicCombo,!g_MicDevs.empty());
        return 0;
    case WM_TIMER:
        InvalidateRect(hwnd,NULL,FALSE); return 0;
    case WM_PAINT:{
        PAINTSTRUCT ps; HDC hdc=BeginPaint(hwnd,&ps);
        RECT rc; GetClientRect(hwnd,&rc);
        std::vector<BYTE> frame;
        { std::lock_guard<std::mutex> lk(g_FrameLock); frame=g_Frame; }
        if(!frame.empty()){
            HGLOBAL hMem=GlobalAlloc(GMEM_MOVEABLE,frame.size());
            void* p=GlobalLock(hMem); memcpy(p,frame.data(),frame.size()); GlobalUnlock(hMem);
            IStream* strm=0; CreateStreamOnHGlobal(hMem,TRUE,&strm);
            Gdiplus::Bitmap* bmp=Gdiplus::Bitmap::FromStream(strm);
            if(bmp&&bmp->GetLastStatus()==Gdiplus::Ok){
                int bw=bmp->GetWidth(),bh=bmp->GetHeight(),pad=10;
                int ax=PANEL_W+pad,ay=pad,aw=rc.right-ax-pad,ah=rc.bottom-ay-pad;
                if(aw>0&&ah>0&&bw>0&&bh>0){
                    float scale=min((float)aw/bw,(float)ah/bh);
                    int dw=(int)(bw*scale),dh=(int)(bh*scale);
                    int dx=ax+(aw-dw)/2,dy=ay+(ah-dh)/2;
                    Gdiplus::Graphics g(hdc);
                    Gdiplus::SolidBrush br(Gdiplus::Color(20,20,24));
                    g.FillRectangle(&br,dx-2,dy-2,dw+4,dh+4);
                    g.DrawImage(bmp,dx,dy,dw,dh);
                }
            }
            delete bmp; strm->Release();
        } else {
            SetBkMode(hdc,TRANSPARENT); SetTextColor(hdc,C_DIM);
            HFONT old=(HFONT)SelectObject(hdc,s_fontLabel);
            const char* txt="Waiting for screen data...";
            TextOutA(hdc,PANEL_W+(rc.right-PANEL_W-220)/2,rc.bottom/2-8,txt,(int)strlen(txt));
            SelectObject(hdc,old);
        }
        EndPaint(hwnd,&ps); return 0;
    }
    case WM_DESTROY: KillTimer(hwnd,1); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd,msg,wp,lp);
}

void FindMP3s(){
    WIN32_FIND_DATAA fd; HANDLE h=FindFirstFileA("*.mp3",&fd);
    if(h==INVALID_HANDLE_VALUE) return;
    do { g_Sounds.push_back(fd.cFileName); } while(FindNextFileA(h,&fd));
    FindClose(h);
}

int main(){
    WSADATA wsa; if(WSAStartup(MAKEWORD(2,2),&wsa)!=0) return 1;

    Gdiplus::GdiplusStartupInput gdip; ULONG_PTR gdipTok;
    Gdiplus::GdiplusStartup(&gdipTok,&gdip,0);

    INITCOMMONCONTROLSEX icex={sizeof(icex),ICC_BAR_CLASSES}; InitCommonControlsEx(&icex);

    FindMP3s();
    std::thread(FrameThread).detach();
    std::thread(AudioReceiverThread).detach();
    std::thread(DevListReceiverThread).detach();

    WNDCLASSEXA wc={sizeof(wc)}; wc.lpfnWndProc=WndProc;
    wc.hInstance=GetModuleHandle(0); wc.hCursor=LoadCursor(0,IDC_ARROW);
    wc.lpszClassName="NyxServer"; RegisterClassExA(&wc);

    g_hWnd=CreateWindowExA(0,"NyxServer","Nyx Remote",
        WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,0,1040,720,0,0,wc.hInstance,0);
    ShowWindow(g_hWnd,SW_SHOW); UpdateWindow(g_hWnd);

    MSG msg;
    while(GetMessage(&msg,0,0,0)){TranslateMessage(&msg);DispatchMessage(&msg);}
    Gdiplus::GdiplusShutdown(gdipTok); WSACleanup(); return 0;
}