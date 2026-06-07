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

const int PORT=4444, PORT_AUDIO=4445, PORT_DEVS=4446, PORT_DIALOG=4447, PORT_PROCS=4448;
const int PANEL_W=264;

#ifndef WAVE_FORMAT_PCM
#define WAVE_FORMAT_PCM 1
#endif

static const GUID SUBTYPE_PCM={0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};

#define MY_GET_X_LPARAM(lp) ((int)(short)(LOWORD(lp)))
#define MY_GET_Y_LPARAM(lp) ((int)(short)(HIWORD(lp)))

static std::vector<BYTE> g_Frame; static std::mutex g_FrameLock;
static SOCKET g_Client=INVALID_SOCKET; static std::mutex g_SockLock;

static HWND g_hWnd,g_hSlider,g_hListBox,g_hPlayBtn,g_hStopBtn,g_hStatus;
static HWND g_hPcAudioChk,g_hMicChk,g_hMicCombo;
static HWND g_hMsgEdit,g_hBtnCombo,g_hBtnEdit1,g_hBtnEdit2,g_hBtnEdit3,g_hSendPopup,g_hResponse;
static HWND g_hFullCtrlChk;
static HWND g_hProcList,g_hProcRefresh,g_hProcKill;
static std::vector<std::string> g_Sounds,g_MicDevs; static std::vector<DWORD> g_ProcPIDs;
static bool g_PcAudioOn=false,g_MicOn=false,g_FullCtrl=false;

static int g_ImgW=0,g_ImgH=0,g_DispX=0,g_DispY=0,g_DispW=0,g_DispH=0;
static bool g_MouseCaptured=false;
static DWORD g_LastMoveTick=0;

static const COLORREF C_PANEL=RGB(38,38,44),C_DISPLAY=RGB(24,24,28),C_LISTBG=RGB(48,48,54);
static const COLORREF C_TEXT=RGB(225,225,230),C_DIM=RGB(110,110,120),C_LINE=RGB(58,58,66);
static const COLORREF C_GREEN=RGB(87,206,117),C_AMBER=RGB(230,160,60),C_CYAN=RGB(80,200,220);
static const COLORREF C_RED=RGB(220,80,80);
static HFONT s_fontTitle,s_fontLabel,s_fontSmall;
static HBRUSH s_brPanel,s_brList,s_brDisplay;

enum { ID_SLIDER=101,ID_LISTBOX=102,ID_PLAY=103,ID_STOP=107,
       ID_PC_AUDIO=104,ID_MIC=105,ID_MIC_COMBO=106,
       ID_MSG_EDIT=108,ID_BTN_COMBO=109,ID_BTN_EDIT1=110,ID_BTN_EDIT2=111,ID_BTN_EDIT3=112,
       ID_SEND_POPUP=113,ID_PROC_LIST=114,ID_PROC_REFRESH=115,ID_PROC_KILL=116,ID_FULL_CTRL=117,
       WM_DEVLIST=WM_USER+1,WM_DIALOG_RESP=WM_USER+2,WM_PROCLIST=WM_USER+3 };

bool RecvAll(SOCKET s,char*d,int len){int t=0;while(t<len){int n=recv(s,d+t,len-t,0);if(n<=0)return false;t+=n;}return true;}

bool SendCmd(char cmd,const char*data=nullptr,int len=0){std::lock_guard<std::mutex>lk(g_SockLock);if(g_Client==INVALID_SOCKET)return false;if(send(g_Client,&cmd,1,0)!=1)return false;if(len>0&&data&&send(g_Client,data,len,0)!=len)return false;return true;}
bool SendVolume(int vol){return SendCmd('V',(char*)&vol,sizeof(vol));}
bool SendPcAudio(bool on){char d=on?1:0;return SendCmd('A',&d,1);}
bool SendStop(){return SendCmd('S');}
bool SendFullCtrl(bool on){char d=on?1:0;return SendCmd('F',&d,1);}
bool SendMic(bool on,int di){std::lock_guard<std::mutex>lk(g_SockLock);if(g_Client==INVALID_SOCKET)return false;char d=on?1:0;if(send(g_Client,"M",1,0)!=1)return false;if(send(g_Client,&d,1,0)!=1)return false;if(on&&send(g_Client,(char*)&di,4,0)!=4)return false;return true;}
bool SendPlay(const std::string&f){std::ifstream in(f,std::ios::binary|std::ios::ate);if(!in)return false;int sz=(int)in.tellg();in.seekg(0);std::vector<char>buf(sz);in.read(buf.data(),sz);std::lock_guard<std::mutex>lk(g_SockLock);if(g_Client==INVALID_SOCKET)return false;if(send(g_Client,"P",1,0)!=1)return false;if(send(g_Client,(char*)&sz,sizeof(sz),0)!=sizeof(sz))return false;if(send(g_Client,buf.data(),sz,0)!=sz)return false;return true;}
bool SendDialog(const std::string&msg,const std::vector<std::string>&btns){std::lock_guard<std::mutex>lk(g_SockLock);if(g_Client==INVALID_SOCKET)return false;if(send(g_Client,"D",1,0)!=1)return false;int ml=(int)msg.size();send(g_Client,(char*)&ml,4,0);if(ml>0)send(g_Client,msg.c_str(),ml,0);int nb=(int)btns.size();send(g_Client,(char*)&nb,4,0);for(auto&b:btns){int bl=(int)b.size();send(g_Client,(char*)&bl,4,0);if(bl>0)send(g_Client,b.c_str(),bl,0);}return true;}
bool SendKill(DWORD pid){return SendCmd('K',(char*)&pid,sizeof(pid));}

bool SendMouseMove(int x,int y){std::lock_guard<std::mutex>lk(g_SockLock);if(g_Client==INVALID_SOCKET)return false;char b[9]={'m'};memcpy(b+1,&x,4);memcpy(b+5,&y,4);return send(g_Client,b,9,0)==9;}
bool SendMouseClick(char btn,char act){std::lock_guard<std::mutex>lk(g_SockLock);if(g_Client==INVALID_SOCKET)return false;char b[3]={'c',btn,act};return send(g_Client,b,3,0)==3;}
bool SendKey(DWORD vk,char act){std::lock_guard<std::mutex>lk(g_SockLock);if(g_Client==INVALID_SOCKET)return false;char b[6]={'k'};memcpy(b+1,&vk,4);b[5]=act;return send(g_Client,b,6,0)==6;}
bool SendMouseWheel(int delta){std::lock_guard<std::mutex>lk(g_SockLock);if(g_Client==INVALID_SOCKET)return false;char b[5]={'w'};memcpy(b+1,&delta,4);return send(g_Client,b,5,0)==5;}

bool MapMouse(LPARAM lp,int&nx,int&ny,bool clamp=false){
    int mx=MY_GET_X_LPARAM(lp),my=MY_GET_Y_LPARAM(lp);
    if(g_DispW<=0||g_DispH<=0)return false;
    if(!clamp){if(mx<g_DispX||mx>g_DispX+g_DispW||my<g_DispY||my>g_DispY+g_DispH)return false;}
    else{if(mx<g_DispX)mx=g_DispX;if(mx>g_DispX+g_DispW)mx=g_DispX+g_DispW;if(my<g_DispY)my=g_DispY;if(my>g_DispY+g_DispH)my=g_DispY+g_DispH;}
    nx=(int)((float)(mx-g_DispX)/g_DispW*10000);ny=(int)((float)(my-g_DispY)/g_DispH*10000);
    if(nx<0)nx=0;if(nx>9999)nx=9999;if(ny<0)ny=0;if(ny>9999)ny=9999;return true;
}

void RefreshStatus(){bool ok;{std::lock_guard<std::mutex>lk(g_SockLock);ok=(g_Client!=INVALID_SOCKET);}if(g_hStatus)SetWindowTextW(g_hStatus,ok?L"\u25CF  Connected":L"\u25CB  Waiting for client\u2026");if(ok){SendCmd('L');g_PcAudioOn=false;g_MicOn=false;if(g_hPcAudioChk)SendMessage(g_hPcAudioChk,BM_SETCHECK,BST_UNCHECKED,0);if(g_hMicChk)SendMessage(g_hMicChk,BM_SETCHECK,BST_UNCHECKED,0);}}

void FrameThread(){SOCKET srv=socket(AF_INET,SOCK_STREAM,0);BOOL r=TRUE;setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,(char*)&r,sizeof(r));sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(PORT);a.sin_addr.s_addr=INADDR_ANY;bind(srv,(sockaddr*)&a,sizeof(a));listen(srv,1);while(true){SOCKET cli=accept(srv,NULL,NULL);if(cli==INVALID_SOCKET)continue;{std::lock_guard<std::mutex>lk(g_SockLock);g_Client=cli;}RefreshStatus();int len;while(RecvAll(cli,(char*)&len,4)&&len>0&&len<10*1024*1024){std::vector<BYTE>buf(len);if(!RecvAll(cli,(char*)buf.data(),len))break;{std::lock_guard<std::mutex>lk(g_FrameLock);g_Frame=std::move(buf);}}{std::lock_guard<std::mutex>lk(g_SockLock);g_Client=INVALID_SOCKET;}closesocket(cli);RefreshStatus();}}

void AudioReceiverThread(){SOCKET srv=socket(AF_INET,SOCK_STREAM,0);BOOL r=TRUE;setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,(char*)&r,sizeof(r));sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(PORT_AUDIO);a.sin_addr.s_addr=INADDR_ANY;bind(srv,(sockaddr*)&a,sizeof(a));listen(srv,1);while(true){SOCKET cli=accept(srv,NULL,NULL);if(cli==INVALID_SOCKET)continue;int sr=48000;short ch=2;if(!RecvAll(cli,(char*)&sr,4)||!RecvAll(cli,(char*)&ch,2)){closesocket(cli);continue;}int inCh=ch;CoInitialize(0);WAVEFORMATEX wfx{};wfx.wFormatTag=WAVE_FORMAT_PCM;wfx.nChannels=ch;wfx.nSamplesPerSec=sr;wfx.wBitsPerSample=16;wfx.nBlockAlign=ch*2;wfx.nAvgBytesPerSec=sr*wfx.nBlockAlign;IMMDeviceEnumerator*pE=NULL;IMMDevice*pD=NULL;IAudioClient*pAC=NULL;IAudioRenderClient*pRC=NULL;HRESULT hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),0,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),(void**)&pE);if(SUCCEEDED(hr))hr=pE->GetDefaultAudioEndpoint(eRender,eConsole,&pD);if(SUCCEEDED(hr))hr=pD->Activate(__uuidof(IAudioClient),CLSCTX_ALL,0,(void**)&pAC);int outCh=ch;if(SUCCEEDED(hr))hr=pAC->Initialize(AUDCLNT_SHAREMODE_SHARED,0,10000000,0,&wfx,NULL);if(FAILED(hr)&&pAC){WAVEFORMATEXTENSIBLE wfex{};wfex.Format.wFormatTag=WAVE_FORMAT_EXTENSIBLE;wfex.Format.nChannels=ch;wfex.Format.nSamplesPerSec=sr;wfex.Format.wBitsPerSample=16;wfex.Format.nBlockAlign=ch*2;wfex.Format.nAvgBytesPerSec=sr*ch*2;wfex.Format.cbSize=22;wfex.Samples.wValidBitsPerSample=16;wfex.dwChannelMask=(ch==1)?0x4:0x3;wfex.SubFormat=SUBTYPE_PCM;hr=pAC->Initialize(AUDCLNT_SHAREMODE_SHARED,0,10000000,0,&wfex.Format,NULL);}if(FAILED(hr)&&pAC){WAVEFORMATEX*pM=NULL;if(SUCCEEDED(pAC->GetMixFormat(&pM))){outCh=pM->nChannels;pM->nSamplesPerSec=sr;pM->nAvgBytesPerSec=sr*pM->nBlockAlign;hr=pAC->Initialize(AUDCLNT_SHAREMODE_SHARED,0,10000000,0,pM,NULL);wfx.nChannels=outCh;wfx.nBlockAlign=outCh*2;wfx.nAvgBytesPerSec=sr*wfx.nBlockAlign;CoTaskMemFree(pM);}}if(SUCCEEDED(hr))hr=pAC->GetService(__uuidof(IAudioRenderClient),(void**)&pRC);if(SUCCEEDED(hr)){UINT32 bf;pAC->GetBufferSize(&bf);pAC->Start();while(true){int len;if(!RecvAll(cli,(char*)&len,4)||len<=0||len>1024*1024)break;std::vector<char>data(len);if(!RecvAll(cli,data.data(),len))break;int inF=len/(inCh*2);std::vector<char>conv;char*pd=data.data();int pl=len;if(inCh!=outCh){int ol=inF*outCh*2;conv.resize(ol);short*iS=(short*)data.data();short*oS=(short*)conv.data();if(inCh==1&&outCh==2){for(int i=0;i<inF;i++){oS[i*2]=iS[i];oS[i*2+1]=iS[i];}}else if(inCh==2&&outCh==1){for(int i=0;i<inF;i++){oS[i]=(short)(((int)iS[i*2]+iS[i*2+1])/2);}}else{for(int i=0;i<inF*outCh&&i<inF*inCh;i++)oS[i]=iS[i];}pd=conv.data();pl=ol;}UINT32 fr=pl/wfx.nBlockAlign;for(int w=0;w<50;w++){UINT32 pad;pAC->GetCurrentPadding(&pad);if(bf-pad>=fr)break;Sleep(2);}UINT32 pad;pAC->GetCurrentPadding(&pad);UINT32 avail=bf-pad;if(fr>avail)fr=avail;if(!fr)continue;BYTE*pO;if(SUCCEEDED(pRC->GetBuffer(fr,&pO))){memcpy(pO,pd,fr*wfx.nBlockAlign);pRC->ReleaseBuffer(fr,0);}}pAC->Stop();}if(pRC)pRC->Release();if(pAC)pAC->Release();if(pD)pD->Release();if(pE)pE->Release();CoUninitialize();closesocket(cli);}}

void DevListReceiverThread(){SOCKET srv=socket(AF_INET,SOCK_STREAM,0);BOOL r=TRUE;setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,(char*)&r,sizeof(r));sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(PORT_DEVS);a.sin_addr.s_addr=INADDR_ANY;bind(srv,(sockaddr*)&a,sizeof(a));listen(srv,1);while(true){SOCKET cli=accept(srv,NULL,NULL);if(cli==INVALID_SOCKET)continue;int c;if(!RecvAll(cli,(char*)&c,4)){closesocket(cli);continue;}std::vector<std::string>d;for(int i=0;i<c&&i<32;i++){int nl;if(!RecvAll(cli,(char*)&nl,4)||nl<0||nl>512)break;if(nl==0){d.push_back("(unknown)");continue;}std::vector<char>nm(nl+1);if(!RecvAll(cli,nm.data(),nl))break;nm[nl]=0;d.push_back(std::string(nm.data()));}g_MicDevs=d;PostMessage(g_hWnd,WM_DEVLIST,0,0);closesocket(cli);}}

void DialogResponseThread(){SOCKET srv=socket(AF_INET,SOCK_STREAM,0);BOOL r=TRUE;setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,(char*)&r,sizeof(r));sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(PORT_DIALOG);a.sin_addr.s_addr=INADDR_ANY;bind(srv,(sockaddr*)&a,sizeof(a));listen(srv,5);while(true){SOCKET cli=accept(srv,NULL,NULL);if(cli==INVALID_SOCKET)continue;int len;if(RecvAll(cli,(char*)&len,4)&&len>0&&len<1024){std::vector<char>buf(len+1);RecvAll(cli,buf.data(),len);buf[len]=0;std::string resp="user clicked "+std::string(buf.data());char*hp=new char[resp.size()+1];strcpy(hp,resp.c_str());PostMessageA(g_hWnd,WM_DIALOG_RESP,0,(LPARAM)hp);}closesocket(cli);}}

void ProcListReceiverThread(){SOCKET srv=socket(AF_INET,SOCK_STREAM,0);BOOL r=TRUE;setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,(char*)&r,sizeof(r));sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(PORT_PROCS);a.sin_addr.s_addr=INADDR_ANY;bind(srv,(sockaddr*)&a,sizeof(a));listen(srv,5);while(true){SOCKET cli=accept(srv,NULL,NULL);if(cli==INVALID_SOCKET)continue;int c;if(!RecvAll(cli,(char*)&c,4)){closesocket(cli);continue;}std::vector<std::string>names;std::vector<DWORD>pids;for(int i=0;i<c&&i<512;i++){DWORD pid;if(!RecvAll(cli,(char*)&pid,4))break;int nl;if(!RecvAll(cli,(char*)&nl,4)||nl<0||nl>256)break;std::string nm;if(nl>0){std::vector<char>buf(nl+1);if(!RecvAll(cli,buf.data(),nl))break;buf[nl]=0;nm=std::string(buf.data());}names.push_back(nm);pids.push_back(pid);}g_ProcPIDs=pids;char**arr=new char*[names.size()];for(size_t i=0;i<names.size();i++){arr[i]=new char[names[i].size()+1];strcpy(arr[i],names[i].c_str());}PostMessage(g_hWnd,WM_PROCLIST,(WPARAM)names.size(),(LPARAM)arr);closesocket(cli);}}

void PlaySelected(){int sel=(int)SendMessageA(g_hListBox,LB_GETCURSEL,0,0);if(sel==LB_ERR||sel>=(int)g_Sounds.size())return;std::string f=g_Sounds[sel];if(f.find('\\')==std::string::npos){char d[MAX_PATH];GetCurrentDirectoryA(MAX_PATH,d);f=std::string(d)+"\\"+f;}SendPlay(f);}
void SendPopup(){char mb[512]={};GetWindowTextA(g_hMsgEdit,mb,512);std::string msg(mb);int c=(int)SendMessageA(g_hBtnCombo,CB_GETCURSEL,0,0)+1;std::vector<std::string>btns;char b1[128]={},b2[128]={},b3[128]={};GetWindowTextA(g_hBtnEdit1,b1,128);btns.push_back(b1);if(c>=2){GetWindowTextA(g_hBtnEdit2,b2,128);btns.push_back(b2);}if(c>=3){GetWindowTextA(g_hBtnEdit3,b3,128);btns.push_back(b3);}SendDialog(msg,btns);}
void KillSelected(){int sel=(int)SendMessageA(g_hProcList,LB_GETCURSEL,0,0);if(sel==LB_ERR||sel>=(int)g_ProcPIDs.size())return;SendKill(g_ProcPIDs[sel]);Sleep(500);SendCmd('R');}

void BuildControls(HWND hwnd){
    s_fontTitle=CreateFontW(22,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    s_fontLabel=CreateFontW(13,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    s_fontSmall=CreateFontW(12,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    s_brPanel=CreateSolidBrush(C_PANEL);s_brList=CreateSolidBrush(C_LISTBG);s_brDisplay=CreateSolidBrush(C_DISPLAY);
    const int x=18,w=PANEL_W-36; int y=14;

    HWND hT=CreateWindowExW(0,L"STATIC",L"NYX REMOTE",WS_CHILD|WS_VISIBLE|SS_LEFT,x,y,w,28,hwnd,0,0,0);SendMessage(hT,WM_SETFONT,(WPARAM)s_fontTitle,0);y+=46;
    HWND hV=CreateWindowExW(0,L"STATIC",L"VOLUME",WS_CHILD|WS_VISIBLE|SS_LEFT,x,y,w,16,hwnd,0,0,0);SendMessage(hV,WM_SETFONT,(WPARAM)s_fontLabel,0);y+=20;
    g_hSlider=CreateWindowExW(0,TRACKBAR_CLASSW,L"",WS_CHILD|WS_VISIBLE|TBS_HORZ|TBS_AUTOTICKS,x,y,w,30,hwnd,(HMENU)ID_SLIDER,0,0);SendMessage(g_hSlider,TBM_SETRANGE,TRUE,MAKELONG(0,100));SendMessage(g_hSlider,TBM_SETPOS,TRUE,50);SendMessage(g_hSlider,TBM_SETTICFREQ,25,0);y+=40;
    HWND hS=CreateWindowExW(0,L"STATIC",L"SOUNDS",WS_CHILD|WS_VISIBLE|SS_LEFT,x,y,w,16,hwnd,0,0,0);SendMessage(hS,WM_SETFONT,(WPARAM)s_fontLabel,0);y+=20;
    g_hListBox=CreateWindowExW(WS_EX_STATICEDGE,L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,x,y,w,140,hwnd,(HMENU)ID_LISTBOX,0,0);SendMessage(g_hListBox,WM_SETFONT,(WPARAM)s_fontSmall,0);SetWindowTheme(g_hListBox,L"",L"");
    for(auto&s:g_Sounds){std::string n=s;auto p=n.find_last_of("\\/");if(p!=std::string::npos)n=n.substr(p+1);SendMessageA(g_hListBox,LB_ADDSTRING,0,(LPARAM)n.c_str());}if(!g_Sounds.empty())SendMessageA(g_hListBox,LB_SETCURSEL,0,0);y+=148;
    int hw=(w-6)/2;g_hPlayBtn=CreateWindowExW(0,L"BUTTON",L"\u25B6  Play",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,x,y,hw,30,hwnd,(HMENU)ID_PLAY,0,0);SendMessage(g_hPlayBtn,WM_SETFONT,(WPARAM)s_fontLabel,0);SetWindowTheme(g_hPlayBtn,L"",L"");
    g_hStopBtn=CreateWindowExW(0,L"BUTTON",L"\u25A0  Stop",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,x+hw+6,y,hw,30,hwnd,(HMENU)ID_STOP,0,0);SendMessage(g_hStopBtn,WM_SETFONT,(WPARAM)s_fontLabel,0);SetWindowTheme(g_hStopBtn,L"",L"");if(g_Sounds.empty())EnableWindow(g_hPlayBtn,FALSE);y+=40;
    HWND hLA=CreateWindowExW(0,L"STATIC",L"PC AUDIO",WS_CHILD|WS_VISIBLE|SS_LEFT,x,y+3,80,16,hwnd,0,0,0);SendMessage(hLA,WM_SETFONT,(WPARAM)s_fontLabel,0);g_hPcAudioChk=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,x+w-20,y+2,20,20,hwnd,(HMENU)ID_PC_AUDIO,0,0);SetWindowTheme(g_hPcAudioChk,L"",L"");y+=28;
    HWND hLM=CreateWindowExW(0,L"STATIC",L"MICROPHONE",WS_CHILD|WS_VISIBLE|SS_LEFT,x,y+3,100,16,hwnd,0,0,0);SendMessage(hLM,WM_SETFONT,(WPARAM)s_fontLabel,0);g_hMicChk=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,x+w-20,y+2,20,20,hwnd,(HMENU)ID_MIC,0,0);SetWindowTheme(g_hMicChk,L"",L"");y+=28;
    g_hMicCombo=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,x,y,w,200,hwnd,(HMENU)ID_MIC_COMBO,0,0);SendMessage(g_hMicCombo,WM_SETFONT,(WPARAM)s_fontSmall,0);SetWindowTheme(g_hMicCombo,L"",L"");y+=36;

    HWND hFC=CreateWindowExW(0,L"STATIC",L"FULL CONTROL",WS_CHILD|WS_VISIBLE|SS_LEFT,x,y+3,110,16,hwnd,0,0,0);SendMessage(hFC,WM_SETFONT,(WPARAM)s_fontLabel,0);
    g_hFullCtrlChk=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,x+w-20,y+2,20,20,hwnd,(HMENU)ID_FULL_CTRL,0,0);SetWindowTheme(g_hFullCtrlChk,L"",L"");y+=28;

    g_hStatus=CreateWindowExW(0,L"STATIC",L"\u25CB  Waiting for client\u2026",WS_CHILD|WS_VISIBLE|SS_LEFT,x,y,w,18,hwnd,0,0,0);SendMessage(g_hStatus,WM_SETFONT,(WPARAM)s_fontSmall,0);y+=26;
    HWND hLP=CreateWindowExW(0,L"STATIC",L"POPUP",WS_CHILD|WS_VISIBLE|SS_LEFT,x,y,w,16,hwnd,0,0,0);SendMessage(hLP,WM_SETFONT,(WPARAM)s_fontLabel,0);y+=20;
    g_hMsgEdit=CreateWindowExW(WS_EX_STATICEDGE,L"EDIT",L"Hi!",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,x,y,w,22,hwnd,(HMENU)ID_MSG_EDIT,0,0);SendMessage(g_hMsgEdit,WM_SETFONT,(WPARAM)s_fontSmall,0);y+=28;
    HWND hBL=CreateWindowExW(0,L"STATIC",L"Buttons:",WS_CHILD|WS_VISIBLE|SS_LEFT,x,y+3,55,16,hwnd,0,0,0);SendMessage(hBL,WM_SETFONT,(WPARAM)s_fontSmall,0);g_hBtnCombo=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,x+58,y,w-58,100,hwnd,(HMENU)ID_BTN_COMBO,0,0);SendMessage(g_hBtnCombo,WM_SETFONT,(WPARAM)s_fontSmall,0);SetWindowTheme(g_hBtnCombo,L"",L"");SendMessageA(g_hBtnCombo,CB_ADDSTRING,0,(LPARAM)"1");SendMessageA(g_hBtnCombo,CB_ADDSTRING,0,(LPARAM)"2");SendMessageA(g_hBtnCombo,CB_ADDSTRING,0,(LPARAM)"3");SendMessageA(g_hBtnCombo,CB_SETCURSEL,1,0);y+=26;
    g_hBtnEdit1=CreateWindowExW(WS_EX_STATICEDGE,L"EDIT",L"Hello!",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,x,y,w,22,hwnd,(HMENU)ID_BTN_EDIT1,0,0);SendMessage(g_hBtnEdit1,WM_SETFONT,(WPARAM)s_fontSmall,0);y+=24;
    g_hBtnEdit2=CreateWindowExW(WS_EX_STATICEDGE,L"EDIT",L"Bye",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,x,y,w,22,hwnd,(HMENU)ID_BTN_EDIT2,0,0);SendMessage(g_hBtnEdit2,WM_SETFONT,(WPARAM)s_fontSmall,0);y+=24;
    g_hBtnEdit3=CreateWindowExW(WS_EX_STATICEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,x,y,w,22,hwnd,(HMENU)ID_BTN_EDIT3,0,0);SendMessage(g_hBtnEdit3,WM_SETFONT,(WPARAM)s_fontSmall,0);EnableWindow(g_hBtnEdit3,FALSE);y+=28;
    g_hSendPopup=CreateWindowExW(0,L"BUTTON",L"\u2757  Send Popup",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,x,y,w,30,hwnd,(HMENU)ID_SEND_POPUP,0,0);SendMessage(g_hSendPopup,WM_SETFONT,(WPARAM)s_fontLabel,0);SetWindowTheme(g_hSendPopup,L"",L"");y+=36;
    g_hResponse=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFT,x,y,w,18,hwnd,0,0,0);SendMessage(g_hResponse,WM_SETFONT,(WPARAM)s_fontSmall,0);y+=26;
    HWND hPR=CreateWindowExW(0,L"STATIC",L"PROCESSES",WS_CHILD|WS_VISIBLE|SS_LEFT,x,y,w,16,hwnd,0,0,0);SendMessage(hPR,WM_SETFONT,(WPARAM)s_fontLabel,0);y+=20;
    g_hProcList=CreateWindowExW(WS_EX_STATICEDGE,L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,x,y,w,120,hwnd,(HMENU)ID_PROC_LIST,0,0);SendMessage(g_hProcList,WM_SETFONT,(WPARAM)s_fontSmall,0);SetWindowTheme(g_hProcList,L"",L"");y+=128;
    int tw=(w-12)/3;g_hProcRefresh=CreateWindowExW(0,L"BUTTON",L"\u21BB  Refresh",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,x,y,tw*2+6,28,hwnd,(HMENU)ID_PROC_REFRESH,0,0);SendMessage(g_hProcRefresh,WM_SETFONT,(WPARAM)s_fontSmall,0);SetWindowTheme(g_hProcRefresh,L"",L"");
    g_hProcKill=CreateWindowExW(0,L"BUTTON",L"\u2716  Kill",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,x+tw*2+12,y,tw,28,hwnd,(HMENU)ID_PROC_KILL,0,0);SendMessage(g_hProcKill,WM_SETFONT,(WPARAM)s_fontSmall,0);SetWindowTheme(g_hProcKill,L"",L"");
}

LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_CREATE:BuildControls(hwnd);SetTimer(hwnd,1,80,NULL);return 0;
    case WM_ERASEBKGND:{HDC hdc=(HDC)wp;RECT rc;GetClientRect(hwnd,&rc);RECT p={0,0,PANEL_W,rc.bottom},d={PANEL_W,0,rc.right,rc.bottom};FillRect(hdc,&p,s_brPanel);FillRect(hdc,&d,s_brDisplay);HPEN pen=CreatePen(PS_SOLID,1,C_LINE),old=(HPEN)SelectObject(hdc,pen);MoveToEx(hdc,PANEL_W,0,NULL);LineTo(hdc,PANEL_W,rc.bottom);SelectObject(hdc,old);DeleteObject(pen);return 1;}
    case WM_CTLCOLORSTATIC:{HDC hdc=(HDC)wp;HWND ctl=(HWND)lp;if(ctl==g_hStatus){bool ok;{std::lock_guard<std::mutex>lk(g_SockLock);ok=(g_Client!=INVALID_SOCKET);}SetTextColor(hdc,ok?C_GREEN:C_AMBER);}else if(ctl==g_hResponse)SetTextColor(hdc,C_CYAN);else SetTextColor(hdc,C_TEXT);SetBkMode(hdc,TRANSPARENT);return(LRESULT)s_brPanel;}
    case WM_CTLCOLORLISTBOX:{HDC hdc=(HDC)wp;SetTextColor(hdc,C_TEXT);SetBkColor(hdc,C_LISTBG);return(LRESULT)s_brList;}
    case WM_CTLCOLOREDIT:{HDC hdc=(HDC)wp;SetTextColor(hdc,C_TEXT);SetBkColor(hdc,C_LISTBG);return(LRESULT)s_brList;}
    case WM_CTLCOLORBTN:{HDC hdc=(HDC)wp;SetTextColor(hdc,C_TEXT);SetBkColor(hdc,C_PANEL);return(LRESULT)s_brPanel;}
    case WM_HSCROLL:if((HWND)lp==g_hSlider){int p=(int)SendMessage(g_hSlider,TBM_GETPOS,0,0);SendVolume(p);}return 0;
    case WM_COMMAND:{
        int id=LOWORD(wp);
        if(id==ID_PLAY)PlaySelected();else if(id==ID_STOP)SendStop();
        else if(id==ID_LISTBOX&&HIWORD(wp)==LBN_DBLCLK)PlaySelected();
        else if(id==ID_PC_AUDIO){bool on=(SendMessage(g_hPcAudioChk,BM_GETCHECK,0,0)==BST_CHECKED);if(on){SendMessage(g_hMicChk,BM_SETCHECK,BST_UNCHECKED,0);g_MicOn=false;SendMic(false,0);g_PcAudioOn=true;SendPcAudio(true);}else{g_PcAudioOn=false;SendPcAudio(false);}}
        else if(id==ID_MIC){bool on=(SendMessage(g_hMicChk,BM_GETCHECK,0,0)==BST_CHECKED);if(on){SendMessage(g_hPcAudioChk,BM_SETCHECK,BST_UNCHECKED,0);g_PcAudioOn=false;SendPcAudio(false);int di=(int)SendMessage(g_hMicCombo,CB_GETCURSEL,0,0);if(di==CB_ERR)di=0;g_MicOn=true;SendMic(true,di);}else{g_MicOn=false;SendMic(false,0);}}
        else if(id==ID_BTN_COMBO&&HIWORD(wp)==CBN_SELCHANGE){int c=(int)SendMessageA(g_hBtnCombo,CB_GETCURSEL,0,0)+1;EnableWindow(g_hBtnEdit2,c>=2);EnableWindow(g_hBtnEdit3,c>=3);}
        else if(id==ID_SEND_POPUP)SendPopup();
        else if(id==ID_PROC_REFRESH)SendCmd('R');
        else if(id==ID_PROC_KILL)KillSelected();
        else if(id==ID_FULL_CTRL){bool on=(SendMessage(g_hFullCtrlChk,BM_GETCHECK,0,0)==BST_CHECKED);g_FullCtrl=on;SendFullCtrl(on);if(on)SetFocus(hwnd);if(!on&&g_MouseCaptured){ReleaseCapture();g_MouseCaptured=false;}}
        return 0;}
    case WM_DIALOG_RESP:{char*r=(char*)lp;SetWindowTextA(g_hResponse,r);delete[]r;return 0;}
    case WM_PROCLIST:{int c=(int)wp;char**arr=(char**)lp;SendMessageA(g_hProcList,LB_RESETCONTENT,0,0);for(int i=0;i<c;i++){SendMessageA(g_hProcList,LB_ADDSTRING,0,(LPARAM)arr[i]);delete[]arr[i];}delete[]arr;return 0;}
    case WM_DEVLIST:SendMessageA(g_hMicCombo,CB_RESETCONTENT,0,0);for(auto&d:g_MicDevs)SendMessageA(g_hMicCombo,CB_ADDSTRING,0,(LPARAM)d.c_str());if(!g_MicDevs.empty())SendMessageA(g_hMicCombo,CB_SETCURSEL,0,0);EnableWindow(g_hMicChk,!g_MicDevs.empty());EnableWindow(g_hMicCombo,!g_MicDevs.empty());return 0;

    case WM_LBUTTONDOWN:case WM_RBUTTONDOWN:case WM_MBUTTONDOWN:
        if(g_FullCtrl){int nx,ny;if(MapMouse(lp,nx,ny)){SetCapture(hwnd);g_MouseCaptured=true;SetFocus(hwnd);SendMouseMove(nx,ny);char btn=(msg==WM_LBUTTONDOWN)?0:(msg==WM_RBUTTONDOWN)?1:2;SendMouseClick(btn,0);}}break;
    case WM_LBUTTONUP:case WM_RBUTTONUP:case WM_MBUTTONUP:
        if(g_FullCtrl){int nx,ny;MapMouse(lp,nx,ny,true);SendMouseMove(nx,ny);char btn=(msg==WM_LBUTTONUP)?0:(msg==WM_RBUTTONUP)?1:2;SendMouseClick(btn,1);if(g_MouseCaptured){ReleaseCapture();g_MouseCaptured=false;}}break;
    case WM_MOUSEMOVE:
        if(g_FullCtrl){int tx,ty;bool over=MapMouse(lp,tx,ty);if(g_MouseCaptured||over){DWORD now=GetTickCount();if(now-g_LastMoveTick<16)break;g_LastMoveTick=now;int nx,ny;if(MapMouse(lp,nx,ny,g_MouseCaptured))SendMouseMove(nx,ny);}}break;
    case WM_MOUSEWHEEL:
        if(g_FullCtrl){POINT pt={MY_GET_X_LPARAM(lp),MY_GET_Y_LPARAM(lp)};ScreenToClient(hwnd,&pt);LPARAM clp=MAKELPARAM(pt.x,pt.y);int nx,ny;if(MapMouse(clp,nx,ny)){int delta=(short)HIWORD(wp);SendMouseMove(nx,ny);SendMouseWheel(delta);}}break;

    case WM_KEYDOWN:case WM_SYSKEYDOWN:
        if(g_FullCtrl&&!(lp&0x40000000)){SendKey((DWORD)wp,0);}break;
    case WM_KEYUP:case WM_SYSKEYUP:
        if(g_FullCtrl){SendKey((DWORD)wp,1);}break;

    case WM_TIMER:InvalidateRect(hwnd,NULL,FALSE);return 0;
    case WM_PAINT:{
        PAINTSTRUCT ps;HDC hdc=BeginPaint(hwnd,&ps);RECT rc;GetClientRect(hwnd,&rc);
        std::vector<BYTE>frame;{std::lock_guard<std::mutex>lk(g_FrameLock);frame=g_Frame;}
        if(!frame.empty()){HGLOBAL hM=GlobalAlloc(GMEM_MOVEABLE,frame.size());void*p=GlobalLock(hM);memcpy(p,frame.data(),frame.size());GlobalUnlock(hM);IStream*strm=0;CreateStreamOnHGlobal(hM,TRUE,&strm);Gdiplus::Bitmap*bmp=Gdiplus::Bitmap::FromStream(strm);
        if(bmp&&bmp->GetLastStatus()==Gdiplus::Ok){int bw=bmp->GetWidth(),bh=bmp->GetHeight(),pad=10;int ax=PANEL_W+pad,ay=pad,aw=rc.right-ax-pad,ah=rc.bottom-ay-pad;if(aw>0&&ah>0&&bw>0&&bh>0){float sc=min((float)aw/bw,(float)ah/bh);int dw=(int)(bw*sc),dh=(int)(bh*sc);int dx=ax+(aw-dw)/2,dy=ay+(ah-dh)/2;
        g_ImgW=bw;g_ImgH=bh;g_DispX=dx;g_DispY=dy;g_DispW=dw;g_DispH=dh;
        Gdiplus::Graphics g(hdc);Gdiplus::SolidBrush br(Gdiplus::Color(20,20,24));g.FillRectangle(&br,dx-2,dy-2,dw+4,dh+4);g.DrawImage(bmp,dx,dy,dw,dh);
        if(g_FullCtrl){Gdiplus::Pen pen(Gdiplus::Color(220,80,80),2);g.DrawRectangle(&pen,dx-1,dy-1,dw+2,dh+2);}}}delete bmp;strm->Release();}
        else{g_ImgW=0;SetBkMode(hdc,TRANSPARENT);SetTextColor(hdc,C_DIM);HFONT old=(HFONT)SelectObject(hdc,s_fontLabel);const char*t="Waiting for screen data...";TextOutA(hdc,PANEL_W+(rc.right-PANEL_W-220)/2,rc.bottom/2-8,t,(int)strlen(t));SelectObject(hdc,old);}
        EndPaint(hwnd,&ps);return 0;}
    case WM_DESTROY:KillTimer(hwnd,1);PostQuitMessage(0);return 0;
    }
    return DefWindowProc(hwnd,msg,wp,lp);
}

void FindMP3s(){WIN32_FIND_DATAA fd;HANDLE h=FindFirstFileA("*.mp3",&fd);if(h==INVALID_HANDLE_VALUE)return;do{g_Sounds.push_back(fd.cFileName);}while(FindNextFileA(h,&fd));FindClose(h);}

int main(){
    WSADATA wsa;if(WSAStartup(MAKEWORD(2,2),&wsa)!=0)return 1;Gdiplus::GdiplusStartupInput gdip;ULONG_PTR gt;Gdiplus::GdiplusStartup(&gt,&gdip,0);INITCOMMONCONTROLSEX icex={sizeof(icex),ICC_BAR_CLASSES};InitCommonControlsEx(&icex);FindMP3s();
    std::thread(FrameThread).detach();std::thread(AudioReceiverThread).detach();std::thread(DevListReceiverThread).detach();std::thread(DialogResponseThread).detach();std::thread(ProcListReceiverThread).detach();
    WNDCLASSEXA wc={sizeof(wc)};wc.lpfnWndProc=WndProc;wc.hInstance=GetModuleHandle(0);wc.hCursor=LoadCursor(0,IDC_ARROW);wc.lpszClassName="NyxServer";RegisterClassExA(&wc);
    g_hWnd=CreateWindowExA(0,"NyxServer","Nyx Remote",WS_OVERLAPPEDWINDOW|WS_VSCROLL,CW_USEDEFAULT,0,1040,960,0,0,wc.hInstance,0);ShowWindow(g_hWnd,SW_SHOW);UpdateWindow(g_hWnd);
    MSG msg;while(GetMessage(&msg,0,0,0)){TranslateMessage(&msg);DispatchMessage(&msg);}Gdiplus::GdiplusShutdown(gt);WSACleanup();return 0;
}