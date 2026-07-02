// =============================================================================
//  Desktop Icon Sphere  —  桌面图标旋转球形
//  全屏覆盖层（HWND_BOTTOM）；图标隐藏；双击打开；可拖拽旋转。
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <gdiplus.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>

#include "resource.h"

// MinGW 缺少 IImageList 完整定义，手动声明 vtable 只调用 GetIcon
static const GUID IID_IImageList={0x46EB5926,0x582E,0x4017,{0x9F,0xDF,0xE8,0x99,0x8D,0xAA,0x09,0x50}};
#ifndef SHIL_JUMBO
#define SHIL_JUMBO 0x4
#endif

#pragma comment(lib,"user32.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"gdiplus.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"shlwapi.lib")
#pragma comment(lib,"uuid.lib")
#pragma comment(lib,"d2d1.lib")
#pragma comment(lib,"dwrite.lib")
#pragma comment(lib,"dwmapi.lib")

#define WM_TRAYICON (WM_APP+1)

// =============================================================================
// 类型
// =============================================================================
struct Vec3 { float x,y,z; };
enum SpinDir    { D_H=0, D_V=1, D_D=2, D_W=3 };

struct Cfg {
    float  spd  = 0.3f;
    SpinDir dir = D_H;
    bool   rev  = false;
    int    ssz  = 68;        // 1.5x 放大后
    int    tilt = 20;
    int    isz  = 72;        // 1.5x 放大后
    int    opa  = 75;        // 降低默认亮度
    int    fade = 55;
    int    fps  = 30;
    bool   autoR= true;
    bool   autob= false;
    bool   showNamesAll= false;
    bool   showFolderN = true;
    bool   showFileN   = true;
    bool   showAppN    = true;
    bool   nameOnHover = false;
    int    nameFontSz  = 10;
    int    nameColor   = 0xFFFFFF;
    int    folderNC    = 0xFFFF00;
    int    appNC       = 0x00FFFF;
    int    pulseMode  = 0;
    bool   dragReverse= false;
    int    blankMode  = 0;    // 0=关闭 1=仅球体空白 2=整个屏幕
};
struct IconBmp { Gdiplus::Bitmap* b=nullptr; int w=0,h=0; std::wstring path; std::wstring name; bool isFolder=false; bool isApp=false; };
struct D2DIcon { ID2D1Bitmap* bmp=nullptr; int w=0,h=0; };
struct IconSnap { int x,y; };
struct IconRect { int idx; RECT rc; };

// =============================================================================
// 全局
// =============================================================================
static HWND             g_hHidden=nullptr, g_hOver=nullptr;
static HMENU            g_hMenu=nullptr;
static NOTIFYICONDATAW  g_nid={};

static Cfg              g_cfg;
static CRITICAL_SECTION g_csCfg, g_csAng, g_csRc, g_csIcons;

static HANDLE           g_hThr=nullptr;
static std::atomic<bool> g_run{false}, g_pause{false}, g_quit{false};
static std::atomic<bool> g_renderSetZ{false};  // 渲染线程正在设置 Z 序
static bool             g_active=false;

// ListView
static HWND    g_hLV=nullptr;
static DWORD   g_pidEx=0;
static HANDLE  g_hPrEx=nullptr;
static std::vector<IconSnap> g_snap;
static int     g_nIcon=0;

// 鼠标
static float   g_ax=0, g_ay=0;
static std::atomic<bool> g_drag{false};
static int     g_drx=0, g_dry=0;
static float   g_dax=0, g_day=0;
static int     g_lcI=-1;
static DWORD   g_lcT=0;

// 渲染
static std::vector<IconRect> g_rects;
static std::vector<IconBmp>  g_icons;
static std::vector<Vec3>     g_pos;
static ULONG_PTR             g_gdiTok=0;

static const wchar_t* kIni=L"icon_sphere.ini";
static const wchar_t* kBak=L"icon_layout_backup.txt";
static HANDLE g_hMutex=nullptr;  // 单实例互斥体

// =============================================================================
// 工具
// =============================================================================
static std::wstring exeDir(){
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr,p,MAX_PATH);
    std::wstring s(p);
    auto i=s.find_last_of(L"\\/");
    return i==std::wstring::npos?s:s.substr(0,i+1);
}
static int clamp(int v,int lo,int hi){return v<lo?lo:(v>hi?hi:v);}
static void setTip(const wchar_t* s){
    int n=sizeof(g_nid.szTip)/sizeof(WCHAR);
    wcsncpy(g_nid.szTip,s,n);g_nid.szTip[n-1]=0;
}

// =============================================================================
// 1. ListView 定位与操作
// =============================================================================
static HWND findDesktop(){
    // 找到真正激活的桌面窗口（含有 SHELLDLL_DefView 的那个）
    HWND pm=FindWindowW(L"Progman",nullptr);
    if(pm){
        HWND dv=FindWindowExW(pm,nullptr,L"SHELLDLL_DefView",nullptr);
        if(dv)return pm;
    }
    HWND w=nullptr;
    while((w=FindWindowExW(nullptr,w,L"WorkerW",nullptr))){
        HWND dv=FindWindowExW(w,nullptr,L"SHELLDLL_DefView",nullptr);
        if(dv)return w;
    }
    return nullptr;
}
static HWND findLV(){
    HWND pm=FindWindowW(L"Progman",nullptr);
    if(pm){
        HWND dv=FindWindowExW(pm,nullptr,L"SHELLDLL_DefView",nullptr);
        if(dv){
            HWND lv=FindWindowExW(dv,nullptr,L"SysListView32",nullptr);
            if(lv)return lv;
        }
    }
    // 枚举 WorkerW
    HWND w=nullptr;
    while((w=FindWindowExW(nullptr,w,L"WorkerW",nullptr))){
        HWND dv=FindWindowExW(w,nullptr,L"SHELLDLL_DefView",nullptr);
        if(dv){
            HWND lv=FindWindowExW(dv,nullptr,L"SysListView32",nullptr);
            if(lv)return lv;
        }
    }
    return nullptr;
}
static bool openProc(){
    if(!g_hLV)return false;
    if(g_hPrEx){CloseHandle(g_hPrEx);g_hPrEx=nullptr;}
    GetWindowThreadProcessId(g_hLV,&g_pidEx);
    if(!g_pidEx)return false;
    g_hPrEx=OpenProcess(PROCESS_VM_OPERATION|PROCESS_VM_READ|PROCESS_VM_WRITE,FALSE,g_pidEx);
    return g_hPrEx!=nullptr;
}
static bool getP(int i,int&x,int&y){
    if(!g_hLV||!g_hPrEx)return false;
    LPVOID r=VirtualAllocEx(g_hPrEx,nullptr,sizeof(POINT),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!r)return false;
    SIZE_T d;POINT z={0};
    WriteProcessMemory(g_hPrEx,r,&z,sizeof(POINT),&d);
    SendMessageTimeoutW(g_hLV,LVM_GETITEMPOSITION,(WPARAM)i,(LPARAM)r,SMTO_ABORTIFHUNG,500,nullptr);
    POINT pt;BOOL ok=ReadProcessMemory(g_hPrEx,r,&pt,sizeof(POINT),&d);
    VirtualFreeEx(g_hPrEx,r,0,MEM_RELEASE);
    if(!ok)return false;
    x=pt.x;y=pt.y;return true;
}
static bool setP(int i,int x,int y){
    if(!g_hLV)return false;
    // 用 LVM_SETITEMPOSITION（坐标在 LPARAM 里，无需跨进程内存）
    return (BOOL)SendMessageTimeoutW(g_hLV,LVM_SETITEMPOSITION,(WPARAM)i,
        MAKELPARAM(x,y),SMTO_ABORTIFHUNG,300,nullptr)!=0;
}
static int cntLV(){return g_hLV?(int)SendMessageW(g_hLV,LVM_GETITEMCOUNT,0,0):0;}

// =============================================================================
// 2. 图标隐藏与恢复（加固版）
// =============================================================================
static bool ensureLV(){
    // 多次重试寻找 ListView
    for(int t=0;t<10;++t){
        if(g_hLV&&IsWindow(g_hLV)){
            if(g_hPrEx)return true;
            if(openProc())return true;
        }
        g_hLV=findLV();
        if(g_hLV){
            if(openProc())return true;
        }
        Sleep(200);
    }
    return false;
}

static bool doSnap(){
    if(!ensureLV())return false;
    int n=cntLV();
    if(n<=0)return false;
    g_nIcon=n;
    g_snap.clear();
    g_snap.reserve(n);
    for(int i=0;i<n;++i){
        int x=-1,y=-1;
        getP(i,x,y);
        g_snap.push_back({x,y});
    }
    std::wstring p=exeDir()+kBak;
    FILE* f=_wfopen(p.c_str(),L"w");
    if(f){
        fwprintf(f,L"# backup\n# count=%d\n",n);
        for(int i=0;i<n;++i)
            fwprintf(f,L"%d %d\n",g_snap[i].x,g_snap[i].y);
        fclose(f);
    }
    return !g_snap.empty();
}

static void doRestore(){
    if(!g_hLV||!IsWindow(g_hLV))g_hLV=findLV();
    if(!g_hLV)return;
    ShowWindow(g_hLV,SW_SHOW);  // 只恢复显示，不重设位置（避免破坏特殊图标）
}

static bool doHide(){
    if(!g_hLV)return false;
    // 直接隐藏整个桌面图标窗口（瞬间生效，无闪烁，不受 auto-arrange 影响）
    return ShowWindow(g_hLV,SW_HIDE)!=0;
}

// =============================================================================
// 3. .lnk 解析 → 目标路径，并提取目标图标
// =============================================================================
static std::wstring resolveLnkTarget(const std::wstring& lnkPath){
    HRESULT hr;
    IShellLinkW* psl=nullptr;
    IPersistFile* ppf=nullptr;

    hr=CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_IShellLinkW,(void**)&psl);
    if(FAILED(hr))return L"";

    hr=psl->QueryInterface(IID_IPersistFile,(void**)&ppf);
    if(FAILED(hr)){psl->Release();return L"";}

    hr=ppf->Load(lnkPath.c_str(),STGM_READ);
    if(FAILED(hr)){ppf->Release();psl->Release();return L"";}

    // 解析快捷方式（允许弹窗交互）
    hr=psl->Resolve(nullptr,SLR_NO_UI|SLR_UPDATE);
    wchar_t target[MAX_PATH]={0};
    hr=psl->GetPath(target,MAX_PATH,nullptr,SLGP_RAWPATH);

    ppf->Release();psl->Release();

    if(SUCCEEDED(hr)&&target[0])return std::wstring(target);
    return L"";
}

// IImageList 精简 vtable（仅用于 GetIcon）
struct IImgLstVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void*,REFIID,void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(void*);
    ULONG   (STDMETHODCALLTYPE *Release)(void*);
    HRESULT (STDMETHODCALLTYPE *Add)(void*,HBITMAP,HBITMAP,int*);
    HRESULT (STDMETHODCALLTYPE *ReplaceIcon)(void*,int,HICON,int*);
    HRESULT (STDMETHODCALLTYPE *SetOverlayImage)(void*,int,int);
    HRESULT (STDMETHODCALLTYPE *Replace)(void*,HIMAGELIST,int);
    HRESULT (STDMETHODCALLTYPE *AddMasked)(void*,HBITMAP,COLORREF,int*);
    HRESULT (STDMETHODCALLTYPE *Draw)(void*,IMAGELISTDRAWPARAMS*);
    HRESULT (STDMETHODCALLTYPE *Remove)(void*,int);
    HRESULT (STDMETHODCALLTYPE *GetIcon)(void*,int,UINT,HICON*);
};
struct IImgLst { IImgLstVtbl* lpVtbl; };

static Gdiplus::Bitmap* getIconForPath(const std::wstring& path){
    // 如果是 .lnk，先解析目标路径
    std::wstring iconPath=path;
    if(path.size()>4){
        std::wstring ext=path.substr(path.size()-4);
        for(auto&ch:ext)ch=(wchar_t)towlower(ch);
        if(ext==L".lnk"){
            std::wstring target=resolveLnkTarget(path);
            if(!target.empty())iconPath=target;
        }
    }

    // 方案A：JUMBO 256×256 超高清图标（4倍超采样 → 缩放到72px = 矢量级清晰）
    Gdiplus::Bitmap* bmp=nullptr;
    HICON hIconForFix=nullptr;
    SHFILEINFOW sfiIdx={};
    if(SHGetFileInfoW(iconPath.c_str(),0,&sfiIdx,sizeof(sfiIdx),SHGFI_SYSICONINDEX)){
        IImgLst* piml=nullptr;
        if(SUCCEEDED(SHGetImageList(SHIL_JUMBO,IID_IImageList,(void**)&piml))&&piml){
            HICON hBig=nullptr;
            if(SUCCEEDED(piml->lpVtbl->GetIcon(piml,sfiIdx.iIcon,ILD_TRANSPARENT,&hBig))&&hBig){
                bmp=Gdiplus::Bitmap::FromHICON(hBig);
                hIconForFix=hBig;  // 保留 HICON 用于黑底修复
            }
            piml->lpVtbl->Release(piml);
        }
    }
    // 方案B：降级 48×48
    if(!bmp){
        SHFILEINFOW sfi={};
        DWORD_PTR ok=SHGetFileInfoW(iconPath.c_str(),0,&sfi,sizeof(sfi),SHGFI_ICON|SHGFI_LARGEICON);
        if(!ok||!sfi.hIcon)ok=SHGetFileInfoW(path.c_str(),0,&sfi,sizeof(sfi),SHGFI_ICON|SHGFI_LARGEICON);
        if(ok&&sfi.hIcon){bmp=Gdiplus::Bitmap::FromHICON(sfi.hIcon);hIconForFix=sfi.hIcon;}
    }
    if(!bmp)return nullptr;

    // 黑底检测（对所有图标生效，包括 JUMBO）
    bool needFix=false;
    if(bmp){
        int iw=bmp->GetWidth(),ih=bmp->GetHeight();
        if(iw>4&&ih>4){
            Gdiplus::Color c;
            bmp->GetPixel(1,1,&c);if(c.GetAlpha()>240)needFix=true;
            if(needFix){bmp->GetPixel(iw-2,1,&c);if(c.GetAlpha()>240)needFix=true;else needFix=false;}
            if(needFix){bmp->GetPixel(1,ih-2,&c);if(c.GetAlpha()>240)needFix=true;else needFix=false;}
            if(needFix){bmp->GetPixel(iw-2,ih-2,&c);if(c.GetAlpha()>240)needFix=true;else needFix=false;}
        } else needFix=true;
    }

    if(bmp&&needFix){
        // 白黑差值法（绝不读 mask bitmap，避开方向 bug）
        delete bmp;bmp=nullptr;
        ICONINFO ii={};
        if(GetIconInfo(hIconForFix,&ii)){
            int iw=0,ih=0;
            BITMAP bm={};
            if(ii.hbmColor){GetObjectW(ii.hbmColor,sizeof(bm),&bm);iw=bm.bmWidth;ih=bm.bmHeight;}
            else if(ii.hbmMask){GetObjectW(ii.hbmMask,sizeof(bm),&bm);iw=bm.bmWidth;ih=bm.bmHeight/2;}
            if(iw>0&&ih>0){
                HDC hdc=GetDC(nullptr);
                BITMAPINFO bi={};bi.bmiHeader.biSize=sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth=iw;bi.bmiHeader.biHeight=-(LONG)ih;
                bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
                BYTE *bitsW=nullptr,*bitsB=nullptr;
                HBITMAP dW=CreateDIBSection(hdc,(BITMAPINFO*)&bi,DIB_RGB_COLORS,(void**)&bitsW,nullptr,0);
                HBITMAP dB=CreateDIBSection(hdc,(BITMAPINFO*)&bi,DIB_RGB_COLORS,(void**)&bitsB,nullptr,0);
                if(dW&&bitsW&&dB&&bitsB){
                    HDC mdc=CreateCompatibleDC(hdc);
                    RECT rc={0,0,iw,ih};
                    HBRUSH wBr=CreateSolidBrush(RGB(255,255,255));
                    HBRUSH bBr=CreateSolidBrush(RGB(0,0,0));
                    HBITMAP old=(HBITMAP)SelectObject(mdc,dW);
                    FillRect(mdc,&rc,wBr);DrawIconEx(mdc,0,0,hIconForFix,iw,ih,0,nullptr,DI_NORMAL);
                    SelectObject(mdc,dB);
                    FillRect(mdc,&rc,bBr);DrawIconEx(mdc,0,0,hIconForFix,iw,ih,0,nullptr,DI_NORMAL);
                    SelectObject(mdc,old);DeleteObject(wBr);DeleteObject(bBr);DeleteDC(mdc);

                    bmp=new Gdiplus::Bitmap(iw,ih,PixelFormat32bppARGB);
                    if(bmp){
                        Gdiplus::BitmapData bd;Gdiplus::Rect rr(0,0,iw,ih);
                        if(bmp->LockBits(&rr,Gdiplus::ImageLockModeWrite,PixelFormat32bppARGB,&bd)==Gdiplus::Ok){
                            BYTE* dst=(BYTE*)bd.Scan0;
                            for(int r=0;r<ih;++r){
                                BYTE* dr=dst+r*bd.Stride,*wr=bitsW+r*iw*4,*br=bitsB+r*iw*4;
                                for(int c=0;c<iw;++c){
                                    int wR=wr[c*4],wG=wr[c*4+1],wB_=wr[c*4+2];
                                    int bR=br[c*4],bG=br[c*4+1],bB_=br[c*4+2];
                                    int dR=wR-bR,dG=wG-bG,dB=wB_-bB_;
                                    int d=dR;if(dG>d)d=dG;if(dB>d)d=dB;
                                    int a=255-d;if(a<0)a=0;if(a>255)a=255;
                                    if(a>0){dr[c*4]=(BYTE)std::min(255,bR*255/a);dr[c*4+1]=(BYTE)std::min(255,bG*255/a);dr[c*4+2]=(BYTE)std::min(255,bB_*255/a);}
                                    else dr[c*4]=dr[c*4+1]=dr[c*4+2]=0;
                                    dr[c*4+3]=(BYTE)a;
                                }
                            }
                            bmp->UnlockBits(&bd);
                        }
                    }
                }
                if(dW)DeleteObject(dW);if(dB)DeleteObject(dB);
                ReleaseDC(nullptr,hdc);
            }
            if(ii.hbmColor)DeleteObject(ii.hbmColor);
            if(ii.hbmMask)DeleteObject(ii.hbmMask);
        }
    }
    if(hIconForFix) DestroyIcon(hIconForFix);
    return bmp;
}

// =============================================================================
// 4. 收集桌面图标
// =============================================================================
static void doCollect(){
    EnterCriticalSection(&g_csIcons);
    for(auto& ib:g_icons)if(ib.b){delete ib.b;ib.b=nullptr;}
    g_icons.clear();
    LeaveCriticalSection(&g_csIcons);

    wchar_t ud[MAX_PATH],pd[MAX_PATH];
    SHGetFolderPathW(nullptr,CSIDL_DESKTOPDIRECTORY,nullptr,0,ud);
    SHGetFolderPathW(nullptr,CSIDL_COMMON_DESKTOPDIRECTORY,nullptr,0,pd);

    std::vector<std::wstring> paths;
    auto en=[&](const wchar_t* dir){
        std::wstring s(dir);s+=L"\\*";
        WIN32_FIND_DATAW fd;
        HANDLE h=FindFirstFileW(s.c_str(),&fd);
        if(h==INVALID_HANDLE_VALUE)return;
        do{
            if(fd.dwFileAttributes&FILE_ATTRIBUTE_HIDDEN)continue;
            if(!wcscmp(fd.cFileName,L".")||!wcscmp(fd.cFileName,L".."))continue;
            std::wstring full(dir);full+=L"\\";full+=fd.cFileName;
            bool dup=false;
            for(auto&p:paths)if(p==full){dup=true;break;}
            if(!dup)paths.push_back(full);
        }while(FindNextFileW(h,&fd));
        FindClose(h);
    };
    en(ud);en(pd);

    EnterCriticalSection(&g_csIcons);
    for(auto& p:paths){
        Gdiplus::Bitmap* bmp=getIconForPath(p);
        if(bmp){
            IconBmp ib;
            ib.b=bmp;ib.w=bmp->GetWidth();ib.h=bmp->GetHeight();ib.path=p;
            // 检测是否为文件夹
            DWORD attr=GetFileAttributesW(p.c_str());
            ib.isFolder=(attr!=INVALID_FILE_ATTRIBUTES&&(attr&FILE_ATTRIBUTE_DIRECTORY));
            ib.isApp=false;
            if(!ib.isFolder){
                std::wstring lower=p;
                for(auto&ch:lower)ch=(wchar_t)towlower(ch);
                if(lower.size()>4){
                    std::wstring ext4=lower.substr(lower.size()-4);
                    if(ext4==L".lnk"||ext4==L".exe")ib.isApp=true;
                }
            }
            // 提取文件名（不含扩展名）
            size_t pos=p.find_last_of(L"\\/");
            ib.name=(pos!=std::wstring::npos)?p.substr(pos+1):p;
            // 去掉 .lnk 后缀
            if(ib.name.size()>4){
                std::wstring ext=ib.name.substr(ib.name.size()-4);
                for(auto&ch:ext)ch=(wchar_t)towlower(ch);
                if(ext==L".lnk")ib.name=ib.name.substr(0,ib.name.size()-4);
            }
            g_icons.push_back(ib);
        }
    }
    LeaveCriticalSection(&g_csIcons);
}

// =============================================================================
// 5. 几何
// =============================================================================
static const float PI=3.14159265358979f;
static Vec3 fib(int i,int n){
    float phi=acosf(1.f-2.f*(i+.5f)/n),th=PI*(1.f+sqrtf(5.f))*(float)i;
    return {sinf(phi)*cosf(th),sinf(phi)*sinf(th),cosf(phi)};
}
static void buildPos(int n){g_pos.resize(n);for(int i=0;i<n;++i)g_pos[i]=fib(i,n);}
static Vec3 rY(const Vec3& v,float a){float c=cosf(a),s=sinf(a);return{c*v.x+s*v.z,v.y,-s*v.x+c*v.z};}
static Vec3 rX(const Vec3& v,float a){float c=cosf(a),s=sinf(a);return{v.x,c*v.y-s*v.z,s*v.y+c*v.z};}
static Vec3 spin(const Vec3& p,float ang,const Cfg& c){
    float d=c.rev?-ang:ang;
    Vec3 r=p;
    float t=(float)c.tilt*PI/180.f;
    switch(c.dir){
        case D_H:r=rY(r,d);r=rX(r,t);break;
        case D_V:r=rX(r,d);r=rY(r,t);break;
        case D_D:r=rX(r,d*.5f);r=rY(r,d);r=rX(r,t);break;
        case D_W:r=rY(r,sinf(d*2.f)*.5f);r=rX(r,sinf(d*1.3f)*.35f+t);break;
    }
    return r;
}

// =============================================================================
// 6. Overlay 窗口（全屏，HWND_BOTTOM）
// =============================================================================
static const float kCR=8.f;  // 圆角半径
static int hitIcon(POINT ptScr){
    EnterCriticalSection(&g_csRc);
    for(int i=(int)g_rects.size()-1;i>=0;--i){
        if(ptScr.x>=g_rects[i].rc.left&&ptScr.x<g_rects[i].rc.right&&
           ptScr.y>=g_rects[i].rc.top&&ptScr.y<g_rects[i].rc.bottom){
            int idx=g_rects[i].idx;
            LeaveCriticalSection(&g_csRc);
            return idx;
        }
    }
    LeaveCriticalSection(&g_csRc);
    return -1;
}

// 判断屏幕坐标点是否被其他窗口遮挡：该点最顶层窗口不是我们的 overlay 即视为遮挡。
// 用于让被遮挡的图标区域对鼠标“失明”（不停转、不显示名字、不响应双击）。
static bool isPointOccluded(POINT pt){
    return WindowFromPhysicalPoint(pt)!=g_hOver;
}

static LRESULT CALLBACK ovrWndProc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
    case WM_WINDOWPOSCHANGING:{
        WINDOWPOS* wp=(WINDOWPOS*)l;
        if(wp->flags&SWP_HIDEWINDOW){
            wp->flags&=~SWP_HIDEWINDOW;
            return 0;
        }
        // 只拒绝外部 Z 序修改；渲染线程自身的 SetWindowPos 放行
        if(!g_renderSetZ && !(wp->flags&SWP_NOZORDER)){
            wp->hwndInsertAfter=HWND_BOTTOM;
            wp->flags|=SWP_NOZORDER;
        }
        break;
    }
    case WM_WINDOWPOSCHANGED:{
        // 兜底：如果还是被隐藏了，延迟恢复
        WINDOWPOS* wp=(WINDOWPOS*)l;
        if(wp->flags&SWP_HIDEWINDOW){
            PostMessageW(h,WM_APP,0,0);
        }
        break;
    }
    case WM_SYSCOMMAND:
        if((w&0xFFF0)==SC_MINIMIZE){
            PostMessageW(h,WM_APP,0,0);
            return 0;
        }
        break;
    case WM_SHOWWINDOW:
        if(!w){PostMessageW(h,WM_APP,0,0);return 0;}
        break;
    case WM_TIMER:{
        // 看门狗：Show Desktop 可能不走标准消息，用定时器兜底
        // 注意：用 SW_SHOWNOACTIVATE(4) 而非 SW_SHOWNA(8)——
        // SW_SHOWNA 会保留最小化状态，无法把窗口从 Show Desktop 中救回。
        if(!IsWindowVisible(h)||IsIconic(h)){
            ShowWindow(h,SW_SHOWNOACTIVATE);
        }
        // 校准 Owner（桌面窗口可能切换 Progman↔WorkerW）
        HWND hd=findDesktop();
        if(hd){
            SetWindowLongPtrW(h,GWLP_HWNDPARENT,(LONG_PTR)hd);
        }
        // 每 100ms 校准 Z 序：锁定在激活桌面窗口正上方
        g_renderSetZ=true;
        SetWindowPos(h,hd?hd:HWND_BOTTOM,
                     0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
        g_renderSetZ=false;
        return 0;
    }
    case WM_SIZE:
        if(w==SIZE_MINIMIZED){PostMessageW(h,WM_APP,0,0);return 0;}
        break;
    case WM_APP:{
        ShowWindow(h,SW_SHOWNOACTIVATE);
        HWND hd=findDesktop();
        g_renderSetZ=true;
        SetWindowPos(h,hd?hd:HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
        g_renderSetZ=false;
        return 0;
    }
    case WM_NCHITTEST:{
        POINT pt={GET_X_LPARAM(l),GET_Y_LPARAM(l)};
        // 被其他窗口遮挡的区域：一律穿透，不做任何鼠标响应
        if(isPointOccluded(pt))return HTTRANSPARENT;
        int idx=hitIcon(pt);
        if(idx>=0)return HTCLIENT; // 图标上：可双击/拖拽
        int bm=g_cfg.blankMode;
        if(bm==2)return HTCLIENT; // 整个屏幕都可拖拽
        if(bm==1){
            // 仅球体空白区域可拖拽
            int sw=GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int sh=GetSystemMetrics(SM_CYVIRTUALSCREEN);
            int cx=GetSystemMetrics(SM_XVIRTUALSCREEN)+sw/2;
            int cy=GetSystemMetrics(SM_YVIRTUALSCREEN)+sh/2;
            int mindim=sw<sh?sw:sh;
            int R=(int)((float)mindim*g_cfg.ssz/200.f);
            int dx=pt.x-cx,dy=pt.y-cy;
            if((float)(dx*dx+dy*dy)<(float)(R*R)*1.3f)return HTCLIENT;
        }
        return HTTRANSPARENT; // 其他区域穿透到桌面
    }
    case WM_LBUTTONDBLCLK:{
        POINT pt;GetCursorPos(&pt);
        int idx=hitIcon(pt);
        if(idx>=0){
            EnterCriticalSection(&g_csIcons);
            if(idx<(int)g_icons.size()&&!g_icons[idx].path.empty())
                ShellExecuteW(nullptr,L"open",g_icons[idx].path.c_str(),nullptr,nullptr,SW_SHOW);
            LeaveCriticalSection(&g_csIcons);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:{
        g_drx=GET_X_LPARAM(l);g_dry=GET_Y_LPARAM(l);
        EnterCriticalSection(&g_csAng);g_dax=g_ax;g_day=g_ay;LeaveCriticalSection(&g_csAng);
        g_drag=true;SetCapture(h);
        // 捕获鼠标可能导致 Z 序变化，立刻压回正确位置
        HWND hd=findDesktop();
        SetWindowPos(h,hd?hd:HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
        return 0;
    }
    case WM_MOUSEMOVE:
        if(g_drag){
            int x=GET_X_LPARAM(l),y=GET_Y_LPARAM(l);
            float dx=(float)(x-g_drx)*.005f,dy=(float)(y-g_dry)*.005f;
            if(g_cfg.dragReverse)dx=-dx; // 逆向滑动
            EnterCriticalSection(&g_csAng);
            g_ax=g_dax+dy;g_ay=g_day+dx;
            LeaveCriticalSection(&g_csAng);
        }
        return 0;
    case WM_LBUTTONUP:
        g_drag=false;ReleaseCapture();
        {HWND hd=findDesktop();
        SetWindowPos(h,hd?hd:HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);}
        return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:{
        // 右键：图标上保留，空白区域转发 WM_CONTEXTMENU 到桌面
        POINT pt={GET_X_LPARAM(l),GET_Y_LPARAM(l)};
        ClientToScreen(h,&pt);
        int idx=hitIcon(pt);
        if(idx>=0)return 0;
        if(m==WM_RBUTTONUP){
            HWND hDesk=FindWindowW(L"Progman",nullptr);
            if(!hDesk)hDesk=FindWindowW(L"WorkerW",nullptr);
            if(hDesk){
                HWND hLV=FindWindowExW(FindWindowExW(hDesk,nullptr,L"SHELLDLL_DefView",nullptr),
                                       nullptr,L"SysListView32",nullptr);
                HWND target=hLV?hLV:hDesk;
                // WM_CONTEXTMENU 使用屏幕坐标
                PostMessageW(target,WM_CONTEXTMENU,(WPARAM)target,MAKELPARAM(pt.x,pt.y));
            }
        }
        return 0;
    }
    case WM_MBUTTONDOWN:
        EnterCriticalSection(&g_csAng);g_ax=0;g_ay=0;LeaveCriticalSection(&g_csAng);
        return 0;
    }
    return DefWindowProcW(h,m,w,l);
}

// =============================================================================
// 8. Direct2D 渲染线程（硬件加速，丝滑+原版画质）
// =============================================================================
static DWORD WINAPI renderThread(LPVOID){
    Gdiplus::GdiplusStartupInput si;si.GdiplusVersion=1;
    Gdiplus::GdiplusStartup(&g_gdiTok,&si,nullptr);

    HRESULT hrCOM=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    (void)hrCOM;

    doCollect();
    buildPos((int)g_icons.size());
    doHide();

    // 全屏尺寸
    int vsx=GetSystemMetrics(SM_XVIRTUALSCREEN),vsy=GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsw=GetSystemMetrics(SM_CXVIRTUALSCREEN),vsh=GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int dibW=vsw,dibH=vsh;

    // 创建全屏 DIB（D2D 直接画到这里）
    HDC sc=GetDC(nullptr);
    BITMAPINFO bi={};
    bi.bmiHeader.biSize=sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth=dibW;bi.bmiHeader.biHeight=-dibH;
    bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    BYTE* dibBits=nullptr;
    HBITMAP dibBmp=CreateDIBSection(sc,&bi,DIB_RGB_COLORS,(void**)&dibBits,nullptr,0);
    HDC dibDC=CreateCompatibleDC(sc);
    HBITMAP oldDib=(HBITMAP)SelectObject(dibDC,dibBmp);
    ReleaseDC(nullptr,sc);
    if(!dibBmp||!dibBits){if(dibDC)DeleteDC(dibDC);Gdiplus::GdiplusShutdown(g_gdiTok);return 1;}

    // D2D 工厂
    ID2D1Factory* d2dF=nullptr;
    if(FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,&d2dF))||!d2dF){
        SelectObject(dibDC,oldDib);DeleteObject(dibBmp);DeleteDC(dibDC);
        Gdiplus::GdiplusShutdown(g_gdiTok);return 1;
    }

    // D2D DC RenderTarget（直接画到 DIB DC，无需 memcpy！）
    D2D1_RENDER_TARGET_PROPERTIES rtp=D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
    ID2D1DCRenderTarget* rt=nullptr;
    d2dF->CreateDCRenderTarget(&rtp,&rt);
    if(!rt){d2dF->Release();SelectObject(dibDC,oldDib);DeleteObject(dibBmp);DeleteDC(dibDC);
        Gdiplus::GdiplusShutdown(g_gdiTok);return 1;}

    // DirectWrite 工厂
    IDWriteFactory* dwF=nullptr;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),(IUnknown**)&dwF);

    // 转换 GDI+ 图标 → D2D Bitmap
    std::vector<D2DIcon> d2dIcons(g_icons.size());
    for(size_t i=0;i<g_icons.size();++i){
        IconBmp& ib=g_icons[i];
        if(!ib.b)continue;
        int iw=ib.w,ih=ib.h;
        D2D1_BITMAP_PROPERTIES bp=D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
        ID2D1Bitmap* d2db=nullptr;
        rt->CreateBitmap(D2D1::SizeU(iw,ih),bp,&d2db);
        if(d2db){
            Gdiplus::BitmapData bd;Gdiplus::Rect rr(0,0,iw,ih);
            if(ib.b->LockBits(&rr,Gdiplus::ImageLockModeRead,PixelFormat32bppARGB,&bd)==Gdiplus::Ok){
                std::vector<BYTE> premul(iw*ih*4);
                BYTE* src=(BYTE*)bd.Scan0;
                for(int r=0;r<ih;++r){
                    BYTE* sr=src+r*bd.Stride,*dr=premul.data()+r*iw*4;
                    for(int c=0;c<iw;++c){
                        BYTE a=sr[c*4+3];
                        dr[c*4+0]=(BYTE)((int)sr[c*4+0]*a/255); // B premul
                        dr[c*4+1]=(BYTE)((int)sr[c*4+1]*a/255); // G premul
                        dr[c*4+2]=(BYTE)((int)sr[c*4+2]*a/255); // R premul
                        dr[c*4+3]=a;
                    }
                }
                ib.b->UnlockBits(&bd);
                D2D1_RECT_U du=D2D1::RectU(0,0,iw,ih);
                d2db->CopyFromMemory(&du,premul.data(),iw*4);
            }
            d2dIcons[i].bmp=d2db;d2dIcons[i].w=iw;d2dIcons[i].h=ih;
        }
    }

    // overlay 置底显示
    SetWindowPos(g_hOver,HWND_BOTTOM,vsx,vsy,dibW,dibH,SWP_NOACTIVATE|SWP_NOOWNERZORDER);
    ShowWindow(g_hOver,SW_SHOWNA);

    // D2D 画刷
    ID2D1SolidColorBrush* whiteBr=nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1),&whiteBr);
    ID2D1SolidColorBrush* nameBr=nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(1,1,1,0.8f),&nameBr);
    IDWriteTextFormat* nameFmt=nullptr;
    if(dwF)dwF->CreateTextFormat(L"Microsoft YaHei",nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,
        10.f,L"",&nameFmt);
    if(nameFmt)nameFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    LARGE_INTEGER fq,lt,lastAngle;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&lt);
    QueryPerformanceCounter(&lastAngle);
    float ang=0.f;

    // 启动飞入动画
    enum { ASTART=0, ANORM=1 } anim=ASTART;
    float animT=0.f;
    LARGE_INTEGER animStart;QueryPerformanceCounter(&animStart);
    std::vector<Vec3> startPos(g_icons.size());
    for(size_t i=0;i<g_icons.size();++i){
        if(i<g_snap.size())startPos[i]={(float)(g_snap[i].x-vsx),(float)(g_snap[i].y-vsy),0.f};
        else startPos[i]={(float)((rand()%2?vsx-300:vsx+vsw+300)),(float)(vsy+rand()%vsh),0.f};
    }

    while(g_run&&!g_quit){
        Cfg c;EnterCriticalSection(&g_csCfg);c=g_cfg;LeaveCriticalSection(&g_csCfg);
        int ms=(c.fps>0)?1000/c.fps:33;

        if(!g_pause){
            // 启动动画计时
            if(anim==ASTART){
                LARGE_INTEGER nw;QueryPerformanceCounter(&nw);
                animT=(float)((double)(nw.QuadPart-animStart.QuadPart)/(double)fq.QuadPart)/1.2f;
                if(animT>1.f){animT=1.f;anim=ANORM;}
            }

            if(!IsWindowVisible(g_hOver)||IsIconic(g_hOver)){
                ShowWindow(g_hOver,SW_SHOWNA);
            }

            RECT rc={0,0,dibW,dibH};
            rt->BindDC(dibDC,&rc);
            rt->BeginDraw();
            // blankMode 命中测试策略：
            //   0=关闭：全屏 alpha=0  → 只有图标区域可点击
            //   1=球体：球体区 alpha>0 → 球体内空白可拖拽；球体外 alpha=0 → 穿透
            //   2=全屏：全屏 alpha>0 → 任意位置可拖拽
            if(c.blankMode==2){
                rt->Clear(D2D1::ColorF(0,0,0,1.f/255.f));
            }else if(c.blankMode==1){
                rt->Clear(D2D1::ColorF(0,0,0,0.f));
                // 在球体投影区域画一层 alpha=1 的实心椭圆，让命中测试只在此区域内生效
                float sr=std::min(vsw,vsh)*(float)c.ssz/200.f*1.4f;
                ID2D1EllipseGeometry* eg=nullptr;
                d2dF->CreateEllipseGeometry(
                    D2D1::Ellipse({dibW*.5f,dibH*.5f},sr,sr),&eg);
                if(eg){
                    ID2D1SolidColorBrush* bgBr=nullptr;
                    rt->CreateSolidColorBrush(D2D1::ColorF(0,0,0,1.f/255.f),&bgBr);
                    if(bgBr){rt->FillGeometry(eg,bgBr);bgBr->Release();}
                    eg->Release();
                }
            }else{
                rt->Clear(D2D1::ColorF(0,0,0,0.f));
            }

            // --- 球体半径 + 启动动画 ---
            int ref=std::min(vsw,vsh);
            float R=(float)c.ssz/100.f*(float)ref*.5f;
            if(anim==ASTART){
                float et=1.f-(1.f-animT)*(1.f-animT); // easeOut
                R*=et;
            }
            // 脉动模式（缓慢/心脏）
            if(c.pulseMode>0){
                LARGE_INTEGER nw;QueryPerformanceCounter(&nw);
                double elapsed=(double)(nw.QuadPart-lastAngle.QuadPart)/(double)fq.QuadPart;
                float beatSec=0.75f; // 80 BPM
                float phase=fmodf((float)(elapsed/beatSec),1.f);
                float pulse=0.f;
                if(c.pulseMode==1){ // 缓慢模式
                    pulse=sinf(phase*PI);
                }else if(c.pulseMode==2){ // 心脏模式
                    int beatIdx=((int)(elapsed/beatSec))%4;
                    float bp=fmodf(phase*4.f,1.f);
                    if(beatIdx==0||beatIdx==1)pulse=sinf(bp*PI)*0.4f; // 轻跳
                    else if(beatIdx==2)pulse=sinf(bp*PI)*1.4f; // 重跳
                    else pulse=0.f; // 停顿
                }
                R*=(1.f+pulse*0.08f);
            }
            float focal=2.2f,fadeF=(float)c.fade/100.f,opaF=(float)c.opa/100.f;
            int isz=c.isz;
            float ax,ay;
            EnterCriticalSection(&g_csAng);ax=g_ax;ay=g_ay;LeaveCriticalSection(&g_csAng);

            struct DI{int idx;float x,y,s,z;};
            std::vector<DI> items;items.reserve(d2dIcons.size());
            size_t cnt=std::min(d2dIcons.size(),g_pos.size());
            float cxDib=dibW*.5f,cyDib=dibH*.5f;
            for(size_t i=0;i<cnt;++i){
                Vec3 p=spin(g_pos[i],ang,c);p=rX(p,ax);p=rY(p,ay);
                float s=focal/(focal-p.z*.9f);
                float tx=cxDib+p.x*R*s,ty=cyDib+p.y*R*s;
                if(anim==ASTART){
                    float et=1.f-(1.f-animT)*(1.f-animT);
                    tx=startPos[i].x+(tx-startPos[i].x)*et;
                    ty=startPos[i].y+(ty-startPos[i].y)*et;
                }
                items.push_back({(int)i,tx,ty,s,p.z});
            }
            std::stable_sort(items.begin(),items.end(),
                [](const DI&a,const DI&b){return a.z<b.z;});

            std::vector<IconRect> rects;
            for(auto& it:items){
                D2DIcon& di=d2dIcons[it.idx];
                if(!di.bmp)continue;
                float fw=isz*it.s,fh=isz*it.s;
                if(fw<2.f||fh<2.f)continue;
                int idw=std::max(4,(int)roundf(fw));   // 尺寸取整→无缩放宽高保清晰
                int idh=std::max(4,(int)roundf(fh));
                float dx=it.x-fw*.5f,dy=it.y-fh*.5f;  // 浮点位置→丝滑移动

                // 透明度
                float da;
                if(it.z>=-0.1f)da=1.f;else da=1.f+(it.z+0.1f)*fadeF;
                if(da<1.f-fadeF)da=1.f-fadeF;if(da>1.f)da=1.f;
                float a=da*opaF;if(a<.02f)continue;

                IconRect ir;ir.idx=it.idx;
                ir.rc={vsx+(int)dx,vsy+(int)dy,vsx+(int)(dx+idw),vsy+(int)(dy+idh)};
                rects.push_back(ir);

                // --- D2D 绘制圆角图标 ---
                float rr=kCR;if(rr*2>idw)rr=idw*.45f;if(rr*2>idh)rr=idh*.45f;
                ID2D1RoundedRectangleGeometry* geo=nullptr;
                d2dF->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(D2D1::RectF(dx,dy,dx+idw,dy+idh),rr,rr),&geo);
                if(geo){
                    ID2D1Layer* layer=nullptr;
                    rt->CreateLayer(&layer);
                    if(layer){
                        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),geo),layer);
                        rt->DrawBitmap(di.bmp,D2D1::RectF(dx,dy,dx+idw,dy+idh),a,
                                       D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                        rt->PopLayer();
                        layer->Release();
                    }
                    geo->Release();
                }

                // 显示文件名（按类型 + 悬停逻辑）
                IconBmp& ib=g_icons[it.idx];
                bool showLabel=false;
                if(c.showNamesAll&&!ib.name.empty()){
                    if(ib.isFolder&&c.showFolderN)showLabel=true;
                    else if(ib.isApp&&c.showAppN)showLabel=true;
                    else if(!ib.isFolder&&!ib.isApp&&c.showFileN)showLabel=true;
                }
                if(showLabel&&c.nameOnHover){
                    POINT mp;GetCursorPos(&mp);
                    bool hoverThis=(mp.x>=vsx+(int)dx&&mp.x<vsx+(int)(dx+idw)&&
                                    mp.y>=vsy+(int)dy&&mp.y<vsy+(int)(dy+idh));
                    // 被其他窗口遮挡：不显示名字（光标或图标中心任一被遮挡）
                    if(hoverThis){
                        POINT ic={(vsx+(int)dx+vsx+(int)(dx+idw))/2,
                                  (vsy+(int)dy+vsy+(int)(dy+idh))/2};
                        if(isPointOccluded(mp)||isPointOccluded(ic))hoverThis=false;
                    }
                    if(!hoverThis)showLabel=false;
                }
                if(showLabel&&dwF&&nameFmt){
                    int clr=c.nameColor; // 文件默认色
                    if(ib.isFolder)clr=c.folderNC;
                    else if(ib.isApp)clr=c.appNC;
                    IDWriteTextFormat* fmt=nullptr;
                    dwF->CreateTextFormat(L"Microsoft YaHei",nullptr,
                        DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,
                        (float)c.nameFontSz,L"",&fmt);
                    if(fmt){
                        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        float textAlpha=std::min(1.f,a*1.2f);
                        float cr=(float)((clr>>16)&0xFF)/255.f;
                        float cg=(float)((clr>>8)&0xFF)/255.f;
                        float cb=(float)(clr&0xFF)/255.f;
                        nameBr->SetColor(D2D1::ColorF(cr,cg,cb,textAlpha));
                        D2D1_RECT_F tr=D2D1::RectF(dx,dy+idh+1,dx+idw,dy+idh+1+c.nameFontSz+4);
                        rt->DrawText(ib.name.c_str(),(UINT32)ib.name.size(),fmt,tr,nameBr);
                        fmt->Release();
                    }
                }
            }

            rt->EndDraw();
            EnterCriticalSection(&g_csRc);g_rects=rects;LeaveCriticalSection(&g_csRc);

            POINT srcPt={0,0};SIZE sz={dibW,dibH};
            BLENDFUNCTION bf={AC_SRC_OVER,0,255,AC_SRC_ALPHA};
            UpdateLayeredWindow(g_hOver,nullptr,nullptr,&sz,dibDC,&srcPt,0,&bf,ULW_ALPHA);

            // 插入到激活的桌面窗口正上方（桌面之上，所有普通窗口之下）
            HWND hDesk=findDesktop();
            g_renderSetZ=true;
            SetWindowPos(g_hOver,hDesk?hDesk:HWND_BOTTOM,
                         0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
            g_renderSetZ=false;

            // 悬停暂停：只有当光标真正停在 overlay 上（未被遮挡）且命中某个图标时才停
            POINT mp;GetCursorPos(&mp);bool on=false;
            if(!isPointOccluded(mp)){
                for(auto& r:rects)
                    if(mp.x>=r.rc.left&&mp.x<r.rc.right&&mp.y>=r.rc.top&&mp.y<r.rc.bottom){on=true;break;}
            }
            if(!on&&!g_drag){
                LARGE_INTEGER nw;QueryPerformanceCounter(&nw);
                double dt=(double)(nw.QuadPart-lastAngle.QuadPart)/(double)fq.QuadPart;
                if(dt>0.1)dt=0.1;ang+=(float)(dt*0.15*c.spd);lastAngle=nw;
            }
        } else {Sleep(1);}

        LARGE_INTEGER nw;QueryPerformanceCounter(&nw);
        double el=(double)(nw.QuadPart-lt.QuadPart)*1000.0/(double)fq.QuadPart;
        int sl=ms-(int)el;if(sl<1)sl=1;if(sl>ms)sl=ms;Sleep(sl);lt=nw;
    }

    ShowWindow(g_hOver,SW_HIDE);
    for(auto& di:d2dIcons)if(di.bmp)di.bmp->Release();
    if(nameFmt)nameFmt->Release();
    if(nameBr)nameBr->Release();
    if(whiteBr)whiteBr->Release();
    if(rt)rt->Release();
    if(dwF)dwF->Release();
    if(d2dF)d2dF->Release();
    SelectObject(dibDC,oldDib);DeleteObject(dibBmp);DeleteDC(dibDC);

    EnterCriticalSection(&g_csIcons);
    for(auto& ib:g_icons)if(ib.b){delete ib.b;ib.b=nullptr;}
    g_icons.clear();
    LeaveCriticalSection(&g_csIcons);

    if(SUCCEEDED(hrCOM))CoUninitialize();
    Gdiplus::GdiplusShutdown(g_gdiTok);g_gdiTok=0;
    return 0;
}

// =============================================================================
// 9. 启停
// =============================================================================
static void stp(){
    g_quit=true;g_run=false;
    if(g_hThr){WaitForSingleObject(g_hThr,3000);CloseHandle(g_hThr);g_hThr=nullptr;}
    g_quit=false;
}
static void start(){
    stp();g_run=true;
    g_hThr=CreateThread(nullptr,0,renderThread,nullptr,0,nullptr);
}
static void updateTrayTip(const wchar_t*);
static void toggle(){
    if(g_active){
        stp();
        doRestore();
        // 立即隐藏球体——还原到无 exe 运行的原始桌面
        if(g_hOver&&IsWindow(g_hOver))ShowWindow(g_hOver,SW_HIDE);
        g_active=false;
        updateTrayTip(L"桌面图标球形 - 已停止");
    }else{
        start();g_active=true;
        updateTrayTip(L"桌面图标球形 - 运行中");
    }
}

// =============================================================================
// 11. 配置
// =============================================================================
static void loadCfg(){
    std::wstring p=exeDir()+kIni;
    g_cfg.spd =(float)GetPrivateProfileIntW(L"main",L"spd",100,p.c_str())/100.f;
    g_cfg.dir =(SpinDir)GetPrivateProfileIntW(L"main",L"dir",D_H,p.c_str());
    g_cfg.rev =GetPrivateProfileIntW(L"main",L"rev",0,p.c_str())!=0;
    g_cfg.ssz =GetPrivateProfileIntW(L"main",L"ssz",68,p.c_str());
    g_cfg.tilt=GetPrivateProfileIntW(L"main",L"tilt",20,p.c_str());
    g_cfg.isz =GetPrivateProfileIntW(L"main",L"isz",72,p.c_str());
    g_cfg.opa =GetPrivateProfileIntW(L"main",L"opa",75,p.c_str());
    g_cfg.fade=GetPrivateProfileIntW(L"main",L"fade",55,p.c_str());
    int fps=GetPrivateProfileIntW(L"main",L"fps",30,p.c_str());
    static const int fpsTab[]={20,30,45,60,90,120,144,240};
    bool validFps=false;
    for(int f:fpsTab)if(fps==f){validFps=true;break;}
    g_cfg.fps=validFps?fps:30;
    g_cfg.autoR=GetPrivateProfileIntW(L"main",L"autoR",1,p.c_str())!=0;
    g_cfg.autob=GetPrivateProfileIntW(L"main",L"autob",0,p.c_str())!=0;
    g_cfg.showNamesAll=GetPrivateProfileIntW(L"main",L"showNamesAll",0,p.c_str())!=0;
    g_cfg.showFolderN=GetPrivateProfileIntW(L"main",L"showFolderN",1,p.c_str())!=0;
    g_cfg.showFileN=GetPrivateProfileIntW(L"main",L"showFileN",1,p.c_str())!=0;
    g_cfg.showAppN=GetPrivateProfileIntW(L"main",L"showAppN",1,p.c_str())!=0;
    g_cfg.nameOnHover=GetPrivateProfileIntW(L"main",L"nameOnHover",0,p.c_str())!=0;
    g_cfg.folderNC=GetPrivateProfileIntW(L"main",L"folderNC",0xFFFF00,p.c_str());
    g_cfg.appNC=GetPrivateProfileIntW(L"main",L"appNC",0x00FFFF,p.c_str());
    g_cfg.nameFontSz=GetPrivateProfileIntW(L"main",L"nameFSz",10,p.c_str());
    g_cfg.nameColor=GetPrivateProfileIntW(L"main",L"nameClr",0xFFFFFF,p.c_str());
    g_cfg.pulseMode=GetPrivateProfileIntW(L"main",L"pulseMode",0,p.c_str());
    g_cfg.dragReverse=GetPrivateProfileIntW(L"main",L"dragRev",0,p.c_str())!=0;
    g_cfg.blankMode=GetPrivateProfileIntW(L"main",L"blankMode",-1,p.c_str());
    if(g_cfg.blankMode<0){
        // 兼容旧版 blankDrag 键
        g_cfg.blankMode=GetPrivateProfileIntW(L"main",L"blankDrag",0,p.c_str())?2:0;
    }
    if(g_cfg.blankMode<0||g_cfg.blankMode>2)g_cfg.blankMode=0;
    if(g_cfg.spd<.2f)g_cfg.spd=.2f;if(g_cfg.spd>3.f)g_cfg.spd=3.f;
}
static void saveCfg(){
    std::wstring p=exeDir()+kIni;wchar_t b[32];
    auto w=[&](const wchar_t*k,int v){wsprintfW(b,L"%d",v);WritePrivateProfileStringW(L"main",k,b,p.c_str());};
    w(L"spd",(int)(g_cfg.spd*100));w(L"dir",g_cfg.dir);w(L"rev",g_cfg.rev?1:0);
    w(L"ssz",g_cfg.ssz);w(L"tilt",g_cfg.tilt);w(L"isz",g_cfg.isz);
    w(L"opa",g_cfg.opa);w(L"fade",g_cfg.fade);w(L"fps",g_cfg.fps);
    w(L"autoR",g_cfg.autoR?1:0);w(L"autob",g_cfg.autob?1:0);
    w(L"showNamesAll",g_cfg.showNamesAll?1:0);
    w(L"showFolderN",g_cfg.showFolderN?1:0);
    w(L"showFileN",g_cfg.showFileN?1:0);
    w(L"showAppN",g_cfg.showAppN?1:0);
    w(L"nameOnHover",g_cfg.nameOnHover?1:0);
    w(L"folderNC",g_cfg.folderNC);
    w(L"appNC",g_cfg.appNC);
    w(L"nameFSz",g_cfg.nameFontSz);w(L"nameClr",g_cfg.nameColor);
    w(L"pulseMode",g_cfg.pulseMode);
    w(L"dragRev",g_cfg.dragReverse?1:0);w(L"blankMode",g_cfg.blankMode);
}

// =============================================================================
// 12. 开机自启
// =============================================================================
static void setBoot(bool on){
    HKEY k;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0,KEY_SET_VALUE,&k)==ERROR_SUCCESS){
        if(on){
            wchar_t e[MAX_PATH];GetModuleFileNameW(nullptr,e,MAX_PATH);
            RegSetValueExW(k,L"IconSphere",0,REG_SZ,(BYTE*)e,
                           (DWORD)((wcslen(e)+1)*sizeof(wchar_t)));
        }else RegDeleteValueW(k,L"IconSphere");
        RegCloseKey(k);
    }
}

// =============================================================================
// 13. 托盘
// =============================================================================
static void mkTray(){
    memset(&g_nid,0,sizeof(g_nid));
    g_nid.cbSize=sizeof(g_nid);g_nid.hWnd=g_hHidden;g_nid.uID=1;
    g_nid.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;g_nid.uCallbackMessage=WM_TRAYICON;
    g_nid.hIcon=(HICON)LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_APPICON),
                                   IMAGE_ICON,16,16,LR_DEFAULTCOLOR);
    if(!g_nid.hIcon)g_nid.hIcon=LoadIconW(nullptr,MAKEINTRESOURCEW(32512));
    setTip(L"桌面图标球形 - 运行中");
    Shell_NotifyIconW(NIM_DELETE,&g_nid);
    Shell_NotifyIconW(NIM_ADD,&g_nid);
}
static void rmTray(){Shell_NotifyIconW(NIM_DELETE,&g_nid);}
static void updateTrayTip(const wchar_t* s){
    setTip(s);NOTIFYICONDATAW n=g_nid;n.uFlags=NIF_TIP;Shell_NotifyIconW(NIM_MODIFY,&n);
}
static void mkMenu(){
    if(g_hMenu)DestroyMenu(g_hMenu);
    g_hMenu=CreatePopupMenu();
    AppendMenuW(g_hMenu,MF_STRING,IDM_TOGGLE_PAUSE,L"暂停 / 恢复");
    AppendMenuW(g_hMenu,MF_STRING,IDM_SETTINGS,L"设置...");
    AppendMenuW(g_hMenu,MF_STRING,IDM_RESTORE,L"还原 / 启动");
    AppendMenuW(g_hMenu,MF_SEPARATOR,0,nullptr);
    AppendMenuW(g_hMenu,MF_STRING,IDM_EXIT,L"退出 (图标自动还原)");
}
static void showMenu(HWND h){
    if(!g_hMenu)mkMenu();
    POINT pt;GetCursorPos(&pt);SetForegroundWindow(h);
    TrackPopupMenu(g_hMenu,TPM_RIGHTALIGN|TPM_BOTTOMALIGN,pt.x,pt.y,0,h,nullptr);
}

// =============================================================================
// 14. 设置对话框
// =============================================================================
static void sync(HWND h){
    wchar_t b[32];int v;
    v=(int)SendMessageW(GetDlgItem(h,IDC_SPEED),TBM_GETPOS,0,0);
    wsprintfW(b,L"%d.%02dx",v/100,v%100);SetDlgItemTextW(h,IDC_SPEED_LABEL,b);
    v=(int)SendMessageW(GetDlgItem(h,IDC_SPHERE_SIZE),TBM_GETPOS,0,0);
    wsprintfW(b,L"%d%%",v);SetDlgItemTextW(h,IDC_SPHERE_LABEL,b);
    v=(int)SendMessageW(GetDlgItem(h,IDC_TILT),TBM_GETPOS,0,0);
    wsprintfW(b,L"%d deg",v);SetDlgItemTextW(h,IDC_TILT_LABEL,b);
    v=(int)SendMessageW(GetDlgItem(h,IDC_ICON_SIZE),TBM_GETPOS,0,0);
    wsprintfW(b,L"%d px",v);SetDlgItemTextW(h,IDC_ICON_LABEL,b);
    v=(int)SendMessageW(GetDlgItem(h,IDC_OPACITY),TBM_GETPOS,0,0);
    wsprintfW(b,L"%d%%",v);SetDlgItemTextW(h,IDC_OPACITY_LABEL,b);
    v=(int)SendMessageW(GetDlgItem(h,IDC_FADE),TBM_GETPOS,0,0);
    wsprintfW(b,L"%d%%",v);SetDlgItemTextW(h,IDC_FADE_LABEL,b);
    v=(int)SendMessageW(GetDlgItem(h,IDC_NAME_FONTSZ),TBM_GETPOS,0,0);
    wsprintfW(b,L"%dpx",v);SetDlgItemTextW(h,IDC_NAME_FONTSZ_LABEL,b);
}
static void inSld(HWND h,const Cfg& c){
    SendMessageW(GetDlgItem(h,IDC_SPEED),TBM_SETRANGE,TRUE,MAKELONG(20,300));SendMessageW(GetDlgItem(h,IDC_SPEED),TBM_SETPOS,TRUE,(int)(c.spd*100));
    SendMessageW(GetDlgItem(h,IDC_SPHERE_SIZE),TBM_SETRANGE,TRUE,MAKELONG(20,80));SendMessageW(GetDlgItem(h,IDC_SPHERE_SIZE),TBM_SETPOS,TRUE,c.ssz);
    SendMessageW(GetDlgItem(h,IDC_TILT),TBM_SETRANGE,TRUE,MAKELONG(0,45));SendMessageW(GetDlgItem(h,IDC_TILT),TBM_SETPOS,TRUE,c.tilt);
    SendMessageW(GetDlgItem(h,IDC_ICON_SIZE),TBM_SETRANGE,TRUE,MAKELONG(24,96));SendMessageW(GetDlgItem(h,IDC_ICON_SIZE),TBM_SETPOS,TRUE,c.isz);
    SendMessageW(GetDlgItem(h,IDC_OPACITY),TBM_SETRANGE,TRUE,MAKELONG(20,100));SendMessageW(GetDlgItem(h,IDC_OPACITY),TBM_SETPOS,TRUE,c.opa);
    SendMessageW(GetDlgItem(h,IDC_FADE),TBM_SETRANGE,TRUE,MAKELONG(0,100));SendMessageW(GetDlgItem(h,IDC_FADE),TBM_SETPOS,TRUE,c.fade);
}
static void getDlg(HWND h,Cfg& n){n=g_cfg;
    n.spd=(float)SendMessageW(GetDlgItem(h,IDC_SPEED),TBM_GETPOS,0,0)/100.f;
    n.dir=(SpinDir)SendMessageW(GetDlgItem(h,IDC_DIRECTION),CB_GETCURSEL,0,0);
    n.rev=IsDlgButtonChecked(h,IDC_REVERSE)==BST_CHECKED;
    n.ssz=(int)SendMessageW(GetDlgItem(h,IDC_SPHERE_SIZE),TBM_GETPOS,0,0);
    n.tilt=(int)SendMessageW(GetDlgItem(h,IDC_TILT),TBM_GETPOS,0,0);
    n.isz=(int)SendMessageW(GetDlgItem(h,IDC_ICON_SIZE),TBM_GETPOS,0,0);
    n.opa=(int)SendMessageW(GetDlgItem(h,IDC_OPACITY),TBM_GETPOS,0,0);
    n.fade=(int)SendMessageW(GetDlgItem(h,IDC_FADE),TBM_GETPOS,0,0);
    int s=clamp((int)SendMessageW(GetDlgItem(h,IDC_FPS),CB_GETCURSEL,0,0),0,7);
    static const int fpsTab[]={20,30,45,60,90,120,144,240};n.fps=fpsTab[s];
    n.autoR=IsDlgButtonChecked(h,IDC_AUTOSTART_ROT)==BST_CHECKED;
    n.autob=IsDlgButtonChecked(h,IDC_AUTOSTART_BOOT)==BST_CHECKED;
    n.showNamesAll=IsDlgButtonChecked(h,IDC_SHOW_NAMES_ALL)==BST_CHECKED;
    n.showFolderN=IsDlgButtonChecked(h,IDC_SHOW_FOLDER_N)==BST_CHECKED;
    n.showFileN=IsDlgButtonChecked(h,IDC_SHOW_FILE_N)==BST_CHECKED;
    n.showAppN=IsDlgButtonChecked(h,IDC_SHOW_APP_N)==BST_CHECKED;
    n.nameOnHover=IsDlgButtonChecked(h,IDC_NAME_ON_HOVER)==BST_CHECKED;
    n.nameFontSz=(int)SendMessageW(GetDlgItem(h,IDC_NAME_FONTSZ),TBM_GETPOS,0,0);
    static const int cols[]={0xFFFFFF,0xFFFF00,0x00FFFF,0x00FF00,0xFF00FF,0xFF0000};
    int ci=clamp((int)SendMessageW(GetDlgItem(h,IDC_NAME_COLOR),CB_GETCURSEL,0,0),0,5);
    n.nameColor=cols[ci];
    int fci=clamp((int)SendMessageW(GetDlgItem(h,IDC_FOLDER_NC),CB_GETCURSEL,0,0),0,5);
    n.folderNC=cols[fci];
    int aci=clamp((int)SendMessageW(GetDlgItem(h,IDC_APP_NC),CB_GETCURSEL,0,0),0,5);
    n.appNC=cols[aci];
    n.pulseMode=clamp((int)SendMessageW(GetDlgItem(h,IDC_MUSIC_MODE),CB_GETCURSEL,0,0),0,2);
    n.dragReverse=IsDlgButtonChecked(h,IDC_DRAG_REVERSE)==BST_CHECKED;
    n.blankMode=clamp((int)SendMessageW(GetDlgItem(h,IDC_BLANK_DRAG),CB_GETCURSEL,0,0),0,2);
}

static INT_PTR CALLBACK dlgProc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
    case WM_INITDIALOG:{
        HWND hd=GetDlgItem(h,IDC_DIRECTION);
        SendMessageW(hd,CB_ADDSTRING,0,(LPARAM)L"水平旋转");SendMessageW(hd,CB_ADDSTRING,0,(LPARAM)L"垂直翻转");
        SendMessageW(hd,CB_ADDSTRING,0,(LPARAM)L"斜向旋转");SendMessageW(hd,CB_ADDSTRING,0,(LPARAM)L"自由摇摆");
        SendMessageW(hd,CB_SETCURSEL,g_cfg.dir,0);
        HWND hf=GetDlgItem(h,IDC_FPS);
        SendMessageW(hf,CB_ADDSTRING,0,(LPARAM)L"20 FPS");SendMessageW(hf,CB_ADDSTRING,0,(LPARAM)L"30 FPS");
        SendMessageW(hf,CB_ADDSTRING,0,(LPARAM)L"45 FPS");SendMessageW(hf,CB_ADDSTRING,0,(LPARAM)L"60 FPS");
        SendMessageW(hf,CB_ADDSTRING,0,(LPARAM)L"90 FPS");SendMessageW(hf,CB_ADDSTRING,0,(LPARAM)L"120 FPS");
        SendMessageW(hf,CB_ADDSTRING,0,(LPARAM)L"144 FPS");SendMessageW(hf,CB_ADDSTRING,0,(LPARAM)L"240 FPS");
        static const int fpsTab[]={20,30,45,60,90,120,144,240};
        int fs=0;for(int i=0;i<8;++i)if(g_cfg.fps==fpsTab[i]){fs=i;break;}
        SendMessageW(hf,CB_SETCURSEL,fs,0);
        CheckDlgButton(h,IDC_REVERSE,g_cfg.rev?BST_CHECKED:BST_UNCHECKED);
        CheckDlgButton(h,IDC_AUTOSTART_ROT,g_cfg.autoR?BST_CHECKED:BST_UNCHECKED);
        CheckDlgButton(h,IDC_AUTOSTART_BOOT,g_cfg.autob?BST_CHECKED:BST_UNCHECKED);
        CheckDlgButton(h,IDC_SHOW_NAMES_ALL,g_cfg.showNamesAll?BST_CHECKED:BST_UNCHECKED);
        CheckDlgButton(h,IDC_SHOW_FOLDER_N,g_cfg.showFolderN?BST_CHECKED:BST_UNCHECKED);
        CheckDlgButton(h,IDC_SHOW_FILE_N,g_cfg.showFileN?BST_CHECKED:BST_UNCHECKED);
        CheckDlgButton(h,IDC_SHOW_APP_N,g_cfg.showAppN?BST_CHECKED:BST_UNCHECKED);
        CheckDlgButton(h,IDC_NAME_ON_HOVER,g_cfg.nameOnHover?BST_CHECKED:BST_UNCHECKED);
        // 子复选框跟随主开关状态
        if(!g_cfg.showNamesAll){
            EnableWindow(GetDlgItem(h,IDC_SHOW_FOLDER_N),FALSE);
            EnableWindow(GetDlgItem(h,IDC_SHOW_FILE_N),FALSE);
            EnableWindow(GetDlgItem(h,IDC_SHOW_APP_N),FALSE);
            EnableWindow(GetDlgItem(h,IDC_NAME_ON_HOVER),FALSE);
        }
        HWND hpm=GetDlgItem(h,IDC_MUSIC_MODE);
        SendMessageW(hpm,CB_ADDSTRING,0,(LPARAM)L"关闭");
        SendMessageW(hpm,CB_ADDSTRING,0,(LPARAM)L"缓慢脉动");
        SendMessageW(hpm,CB_ADDSTRING,0,(LPARAM)L"心脏模式");
        SendMessageW(hpm,CB_SETCURSEL,g_cfg.pulseMode,0);
        CheckDlgButton(h,IDC_DRAG_REVERSE,g_cfg.dragReverse?BST_CHECKED:BST_UNCHECKED);
        // 空白拖拽模式下拉框
        HWND hbm=GetDlgItem(h,IDC_BLANK_DRAG);
        SendMessageW(hbm,CB_ADDSTRING,0,(LPARAM)L"关闭");
        SendMessageW(hbm,CB_ADDSTRING,0,(LPARAM)L"仅球体空白");
        SendMessageW(hbm,CB_ADDSTRING,0,(LPARAM)L"整个屏幕");
        SendMessageW(hbm,CB_SETCURSEL,g_cfg.blankMode,0);
        // 字体大小滑块
        SendMessageW(GetDlgItem(h,IDC_NAME_FONTSZ),TBM_SETRANGE,TRUE,MAKELONG(6,28));
        SendMessageW(GetDlgItem(h,IDC_NAME_FONTSZ),TBM_SETPOS,TRUE,g_cfg.nameFontSz);
        // 颜色下拉 - 文件默认色
        HWND hcl=GetDlgItem(h,IDC_NAME_COLOR);
        SendMessageW(hcl,CB_ADDSTRING,0,(LPARAM)L"白色");
        SendMessageW(hcl,CB_ADDSTRING,0,(LPARAM)L"黄色");
        SendMessageW(hcl,CB_ADDSTRING,0,(LPARAM)L"青色");
        SendMessageW(hcl,CB_ADDSTRING,0,(LPARAM)L"绿色");
        SendMessageW(hcl,CB_ADDSTRING,0,(LPARAM)L"品红");
        SendMessageW(hcl,CB_ADDSTRING,0,(LPARAM)L"红色");
        static const int cols[]={0xFFFFFF,0xFFFF00,0x00FFFF,0x00FF00,0xFF00FF,0xFF0000};
        int ci=0;for(int i=0;i<6;++i)if(cols[i]==g_cfg.nameColor){ci=i;break;}
        SendMessageW(hcl,CB_SETCURSEL,ci,0);
        // 文件夹颜色
        HWND hfcl=GetDlgItem(h,IDC_FOLDER_NC);
        SendMessageW(hfcl,CB_ADDSTRING,0,(LPARAM)L"白");SendMessageW(hfcl,CB_ADDSTRING,0,(LPARAM)L"黄");
        SendMessageW(hfcl,CB_ADDSTRING,0,(LPARAM)L"青");SendMessageW(hfcl,CB_ADDSTRING,0,(LPARAM)L"绿");
        SendMessageW(hfcl,CB_ADDSTRING,0,(LPARAM)L"紫");SendMessageW(hfcl,CB_ADDSTRING,0,(LPARAM)L"红");
        int fci=0;for(int i=0;i<6;++i)if(cols[i]==g_cfg.folderNC){fci=i;break;}
        SendMessageW(hfcl,CB_SETCURSEL,fci,0);
        // 应用颜色
        HWND hacl=GetDlgItem(h,IDC_APP_NC);
        SendMessageW(hacl,CB_ADDSTRING,0,(LPARAM)L"白");SendMessageW(hacl,CB_ADDSTRING,0,(LPARAM)L"黄");
        SendMessageW(hacl,CB_ADDSTRING,0,(LPARAM)L"青");SendMessageW(hacl,CB_ADDSTRING,0,(LPARAM)L"绿");
        SendMessageW(hacl,CB_ADDSTRING,0,(LPARAM)L"紫");SendMessageW(hacl,CB_ADDSTRING,0,(LPARAM)L"红");
        int aci=0;for(int i=0;i<6;++i)if(cols[i]==g_cfg.appNC){aci=i;break;}
        SendMessageW(hacl,CB_SETCURSEL,aci,0);
        inSld(h,g_cfg);sync(h);
        return TRUE;
    }
    case WM_HSCROLL:case WM_VSCROLL:sync(h);return TRUE;
    case WM_COMMAND:
        switch(LOWORD(w)){
        case IDC_SHOW_NAMES_ALL:{
            BOOL en=IsDlgButtonChecked(h,IDC_SHOW_NAMES_ALL)==BST_CHECKED;
            EnableWindow(GetDlgItem(h,IDC_SHOW_FOLDER_N),en);
            EnableWindow(GetDlgItem(h,IDC_SHOW_FILE_N),en);
            EnableWindow(GetDlgItem(h,IDC_SHOW_APP_N),en);
            EnableWindow(GetDlgItem(h,IDC_NAME_ON_HOVER),en);
            return TRUE;
        }
        case IDC_DEFAULTS:{
            Cfg d;
            SendMessageW(GetDlgItem(h,IDC_DIRECTION),CB_SETCURSEL,d.dir,0);
            CheckDlgButton(h,IDC_REVERSE,d.rev?BST_CHECKED:BST_UNCHECKED);
            CheckDlgButton(h,IDC_AUTOSTART_ROT,d.autoR?BST_CHECKED:BST_UNCHECKED);
            CheckDlgButton(h,IDC_AUTOSTART_BOOT,d.autob?BST_CHECKED:BST_UNCHECKED);
            CheckDlgButton(h,IDC_SHOW_NAMES_ALL,d.showNamesAll?BST_CHECKED:BST_UNCHECKED);
            CheckDlgButton(h,IDC_SHOW_FOLDER_N,d.showFolderN?BST_CHECKED:BST_UNCHECKED);
            CheckDlgButton(h,IDC_SHOW_FILE_N,d.showFileN?BST_CHECKED:BST_UNCHECKED);
            CheckDlgButton(h,IDC_SHOW_APP_N,d.showAppN?BST_CHECKED:BST_UNCHECKED);
            CheckDlgButton(h,IDC_NAME_ON_HOVER,d.nameOnHover?BST_CHECKED:BST_UNCHECKED);
            EnableWindow(GetDlgItem(h,IDC_SHOW_FOLDER_N),FALSE);
            EnableWindow(GetDlgItem(h,IDC_SHOW_FILE_N),FALSE);
            EnableWindow(GetDlgItem(h,IDC_SHOW_APP_N),FALSE);
            EnableWindow(GetDlgItem(h,IDC_NAME_ON_HOVER),FALSE);
            SendMessageW(GetDlgItem(h,IDC_MUSIC_MODE),CB_SETCURSEL,d.pulseMode,0);
            CheckDlgButton(h,IDC_DRAG_REVERSE,d.dragReverse?BST_CHECKED:BST_UNCHECKED);
            SendMessageW(GetDlgItem(h,IDC_BLANK_DRAG),CB_SETCURSEL,d.blankMode,0);
            SendMessageW(GetDlgItem(h,IDC_NAME_FONTSZ),TBM_SETPOS,TRUE,d.nameFontSz);
            SendMessageW(GetDlgItem(h,IDC_NAME_COLOR),CB_SETCURSEL,0,0);
            SendMessageW(GetDlgItem(h,IDC_FOLDER_NC),CB_SETCURSEL,1,0);  // 黄色
            SendMessageW(GetDlgItem(h,IDC_APP_NC),CB_SETCURSEL,2,0);     // 青色
            inSld(h,d);sync(h);return TRUE;
        }
        case IDC_APPLY:case IDC_OK:{
            Cfg n;getDlg(h,n);
            EnterCriticalSection(&g_csCfg);g_cfg=n;LeaveCriticalSection(&g_csCfg);
            saveCfg();setBoot(n.autob);
            if(LOWORD(w)==IDC_OK)EndDialog(h,IDOK);
            return TRUE;
        }
        case IDC_CANCEL:EndDialog(h,IDCANCEL);return TRUE;
        }break;
    case WM_CLOSE:EndDialog(h,IDCANCEL);return TRUE;
    }
    (void)l;return FALSE;
}

static void openSettings(HWND parent){
    DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_SETTINGS),
                    parent,dlgProc,0);
}

// =============================================================================
// 15. 隐藏窗口
// =============================================================================
static LRESULT CALLBACK hidWndProc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
    case WM_TRAYICON:
        if(l==WM_RBUTTONUP)showMenu(h);
        else if(l==WM_LBUTTONUP){
            g_pause=!g_pause;
            updateTrayTip(g_pause?L"桌面图标球形 - 已暂停":L"桌面图标球形 - 运行中");
        }
        else if(l==WM_LBUTTONDBLCLK)openSettings(h);
        return 0;
    case WM_COMMAND:
        switch(LOWORD(w)){
        case IDM_TOGGLE_PAUSE:
            g_pause=!g_pause;
            updateTrayTip(g_pause?L"桌面图标球形 - 已暂停":L"桌面图标球形 - 运行中");return 0;
        case IDM_SETTINGS:openSettings(h);return 0;
        case IDM_RESTORE:toggle();return 0;
        case IDM_EXIT:PostMessageW(h,WM_CLOSE,0,0);return 0;
        }break;
    case WM_QUERYENDSESSION:case WM_CLOSE:
        stp();doRestore();rmTray();PostQuitMessage(0);
        return(m==WM_QUERYENDSESSION)?TRUE:0;
    }
    return DefWindowProcW(h,m,w,l);
}

// =============================================================================
// 16. 入口
// =============================================================================
int WINAPI WinMain(HINSTANCE hI,HINSTANCE,LPSTR,int){
    // 单实例检测：防止重复打开
    g_hMutex=CreateMutexW(nullptr,FALSE,L"IconSphere_SingleInstance_Mutex");
    if(g_hMutex&&GetLastError()==ERROR_ALREADY_EXISTS){
        CloseHandle(g_hMutex);g_hMutex=nullptr;
        // 已有实例运行 → 弹出简约提示后退出
        HWND hExisting=FindWindowW(L"IconSphereH",L"IconSphere");
        if(hExisting)SetForegroundWindow(hExisting);
        // 简约横幅通知
        HWND hNotify=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_LAYERED,
            L"Static",L"Icon Sphere  is already running",
            WS_POPUP|WS_VISIBLE|SS_CENTER|SS_CENTERIMAGE,
            0,0,340,40,0,0,hI,0);
        if(hNotify){
            // 深色背景+白色文字
            SetWindowLongPtrW(hNotify,GWLP_USERDATA,(LONG_PTR)CreateSolidBrush(RGB(35,35,35)));
            int sw=GetSystemMetrics(SM_CXSCREEN),sh=GetSystemMetrics(SM_CYSCREEN);
            SetWindowPos(hNotify,0,(sw-340)/2,sh/4,340,40,SWP_NOZORDER);
            SetLayeredWindowAttributes(hNotify,0,230,LWA_ALPHA);
            SetTimer(hNotify,1,2000,nullptr);
            MSG m;while(GetMessageW(&m,0,0,0)>0){
                if(m.message==WM_TIMER){break;}
                if(m.message==WM_CTLCOLORSTATIC){
                    SetTextColor((HDC)m.wParam,RGB(255,255,255));
                    SetBkColor((HDC)m.wParam,RGB(35,35,35));
                    return(LRESULT)GetWindowLongPtrW(hNotify,GWLP_USERDATA);
                }
                TranslateMessage(&m);DispatchMessageW(&m);
            }
            DeleteObject((HBRUSH)GetWindowLongPtrW(hNotify,GWLP_USERDATA));
            DestroyWindow(hNotify);
        }
        return 0;
    }

    InitializeCriticalSection(&g_csCfg);
    InitializeCriticalSection(&g_csAng);
    InitializeCriticalSection(&g_csRc);
    InitializeCriticalSection(&g_csIcons);

    SetProcessDPIAware();
    INITCOMMONCONTROLSEX icc={sizeof(icc),ICC_BAR_CLASSES|ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    loadCfg();

    // 隐藏窗口
    static const wchar_t* kH=L"IconSphereH";
    WNDCLASSW wc={};
    wc.lpfnWndProc=hidWndProc;wc.hInstance=hI;wc.lpszClassName=kH;
    RegisterClassW(&wc);
    g_hHidden=CreateWindowExW(0,kH,L"IconSphere",WS_OVERLAPPEDWINDOW,
                               0,0,0,0,nullptr,nullptr,hI,nullptr);
    ShowWindow(g_hHidden,SW_HIDE);

    // 叠加层（全屏，HWND_BOTTOM）
    int vsx=GetSystemMetrics(SM_XVIRTUALSCREEN),vsy=GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsw=GetSystemMetrics(SM_CXVIRTUALSCREEN),vsh=GetSystemMetrics(SM_CYVIRTUALSCREEN);

    static const wchar_t* kO=L"IconSphereO";
    WNDCLASSW wc2={};
    wc2.lpfnWndProc=ovrWndProc;wc2.hInstance=hI;wc2.lpszClassName=kO;
    wc2.style=CS_DBLCLKS;wc2.hCursor=LoadCursorW(nullptr,(LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc2);
    HWND hOwner=findDesktop(); // 桌面作为 Owner
    g_hOver=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,
        kO,L"",WS_POPUP,vsx,vsy,vsw,vsh,hOwner,nullptr,hI,nullptr);
    // 排除在 Aero Peek / Show Desktop 之外：DWM 合成层永不隐藏此窗口
    BOOL excludePeek=TRUE;
    DwmSetWindowAttribute(g_hOver,DWMWA_EXCLUDED_FROM_PEEK,&excludePeek,sizeof(excludePeek));
    SetTimer(g_hOver,1,100,nullptr);  // 看门狗：每100ms校准可见性和Z序
    SetWindowPos(g_hOver,HWND_BOTTOM,0,0,0,0,
                 SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);

    mkTray();mkMenu();

    // 快照图标位置（始终需要，用于退出还原）
    doSnap();
    // 华丽模式在渲染线程里隐藏 ListView；性能模式保持 ListView 可见（实时移动图标）

    start();g_active=true;

    MSG msg;
    while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}

    g_quit=true;stp();doRestore();rmTray();
    if(g_hOver){DestroyWindow(g_hOver);g_hOver=nullptr;}
    DeleteCriticalSection(&g_csCfg);DeleteCriticalSection(&g_csAng);
    DeleteCriticalSection(&g_csRc);DeleteCriticalSection(&g_csIcons);
    if(g_hMutex)CloseHandle(g_hMutex);
    return 0;
}
