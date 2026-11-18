// PCam integration: publishes frames through the Windows Media Foundation virtual-camera API.
// Uses WIL / C++/WinRT SDK headers; imported sample files are listed separately in ../SOURCES.md.
#include "pch.h"
#include <memory>
#include "PcamDirectShow.h"
#pragma comment(lib,"advapi32")

// MFCreateVirtualCamera only exists on Windows 11 (build 22000+). Resolve it at
// run time so this DLL also loads on Windows 10, where the DirectShow filter
// consumes the same shared frame file. The explicit signature avoids a static
// import of the Windows 11-only export. The function is exported by
// mfsensorgroup.dll (the DLL also imported for the Sensor Profile APIs).
using CreateVirtualCameraFn = HRESULT(WINAPI*)(MFVirtualCameraType,MFVirtualCameraLifetime,MFVirtualCameraAccess,
    LPCWSTR,LPCWSTR,const GUID*,ULONG,IMFVirtualCamera**);
static CreateVirtualCameraFn FindCreateVirtualCamera() noexcept
{
    HMODULE module=GetModuleHandleW(L"mfsensorgroup.dll");
    return module?reinterpret_cast<CreateVirtualCameraFn>(GetProcAddress(module,"MFCreateVirtualCamera")):nullptr;
}

struct Publisher{
    wil::com_ptr_nothrow<IMFVirtualCamera> camera;
    HANDLE file=INVALID_HANDLE_VALUE,mapping=nullptr,mutex=nullptr;
    PcamFrames* view=nullptr;bool mf=false,com=false,owner=false;
    ~Publisher(){
        // Invalidate and wipe the mapped pixels before stopping the camera. A file
        // left by an abnormal exit is stale after two seconds, never replayed.
        if(view){InterlockedIncrement(&view->sequence);view->tick=0;
            SecureZeroMemory(view->pixels,sizeof(view->pixels));InterlockedIncrement(&view->sequence);}
        if(camera){camera->Shutdown();camera.reset();}
        if(view)UnmapViewOfFile(view);if(mapping)CloseHandle(mapping);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
        if(mutex){if(owner)ReleaseMutex(mutex);CloseHandle(mutex);}if(mf)MFShutdown();if(com)CoUninitialize();
    }
};
extern "C" HRESULT __stdcall PcStart(LPCWSTR path,LPCWSTR sid,void** result){
 try{
    if(!path || !sid || !result)return E_POINTER;*result=nullptr;
    auto p=std::make_unique<Publisher>();auto hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(hr) && hr!=RPC_E_CHANGED_MODE)return hr;p->com=SUCCEEDED(hr);
    std::wstring name=L"Local\\PCam.VirtualCamera.Publisher.";name+=sid;
    p->mutex=CreateMutexW(nullptr,TRUE,name.c_str());if(!p->mutex)return HRESULT_FROM_WIN32(GetLastError());
    if(GetLastError()==ERROR_ALREADY_EXISTS)return HRESULT_FROM_WIN32(ERROR_BUSY);p->owner=true;
    // The reader must share our write access, but we do not share write/delete
    // access with other processes. This prevents concurrent truncation of a live mapping.
    p->file=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(p->file==INVALID_HANDLE_VALUE)return HRESULT_FROM_WIN32(GetLastError());
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(p->file,&info))return HRESULT_FROM_WIN32(GetLastError());
    if((info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) || info.nNumberOfLinks!=1)return E_ACCESSDENIED;
    LARGE_INTEGER size{};size.QuadPart=sizeof(PcamFrames);
    LARGE_INTEGER current{};
    if(!GetFileSizeEx(p->file,&current))return HRESULT_FROM_WIN32(GetLastError());
    // Never resize a file that may already be mapped by a DirectShow consumer:
    // Windows rejects SetEndOfFile while another process holds a mapped view.
    // Readers refuse files smaller than PcamFrames, so extending is safe.
    if(current.QuadPart<size.QuadPart){
        if(!SetFilePointerEx(p->file,size,nullptr,FILE_BEGIN)||!SetEndOfFile(p->file))return HRESULT_FROM_WIN32(GetLastError());
    }
    p->mapping=CreateFileMappingW(p->file,nullptr,PAGE_READWRITE,0,0,nullptr);if(!p->mapping)return HRESULT_FROM_WIN32(GetLastError());
    p->view=(PcamFrames*)MapViewOfFile(p->mapping,FILE_MAP_WRITE|FILE_MAP_READ,0,0,sizeof(PcamFrames));
    if(!p->view)return HRESULT_FROM_WIN32(GetLastError());ZeroMemory(p->view,sizeof(PcamFrames));p->view->magic=PCAM_MAGIC;
    // The DirectShow filter runs inside the consuming application; record the
    // per-user frame path where it can find it. Failure only matters on
    // Windows 10, where DirectShow is the only available device path.
    HKEY key=nullptr;LSTATUS status=RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\PCam\\VirtualCamera",0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
    if(status==ERROR_SUCCESS){
        status=RegSetValueExW(key,L"FramePath",0,REG_SZ,reinterpret_cast<const BYTE*>(path),static_cast<DWORD>((wcslen(path)+1)*sizeof(wchar_t)));
        RegCloseKey(key);
    }
    RETURN_IF_FAILED(MFStartup(MF_VERSION));p->mf=true;
    if(auto create=FindCreateVirtualCamera()){
        RETURN_IF_FAILED(create(MFVirtualCameraType_SoftwareCameraSource,MFVirtualCameraLifetime_Session,MFVirtualCameraAccess_CurrentUser,L"PCam",PCAM_CLSID,nullptr,0,&p->camera));
        RETURN_IF_FAILED(p->camera->SetString(PCAM_FRAME_PATH,path));
        RETURN_IF_FAILED(p->camera->Start(nullptr));
    }else if(status!=ERROR_SUCCESS){
        return HRESULT_FROM_WIN32(status);
    }
    *result=p.release();return S_OK;
 }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
}

extern "C" HRESULT __stdcall PcWrite(void* handle,const BYTE* pixels,int size){
    auto p=(Publisher*)handle;if(!p || !p->view || !pixels || size!=PCAM_STRIDE*PCAM_HEIGHT)return E_INVALIDARG;
    auto v=p->view;int slot=1-v->slot;InterlockedIncrement(&v->sequence);
    memcpy(v->pixels[slot],pixels,size);v->tick=GetTickCount64();v->slot=slot;InterlockedIncrement(&v->sequence);return S_OK;
}
extern "C" void __stdcall PcStop(void* handle){delete (Publisher*)handle;}
extern "C" HRESULT __stdcall PcRegister(){
    wchar_t path[32768]{};HMODULE module=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)&PcRegister,&module))return HRESULT_FROM_WIN32(GetLastError());
    if(!GetModuleFileNameW(module,path,32768))return HRESULT_FROM_WIN32(GetLastError());
    std::wstring key=L"SOFTWARE\\Classes\\CLSID\\";key+=PCAM_CLSID;key+=L"\\InprocServer32";
    HKEY reg=nullptr;auto code=RegCreateKeyExW(HKEY_LOCAL_MACHINE,key.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&reg,nullptr);
    if(code!=ERROR_SUCCESS)return HRESULT_FROM_WIN32(code);
    code=RegSetValueExW(reg,nullptr,0,REG_SZ,(BYTE*)path,(DWORD)((wcslen(path)+1)*sizeof(wchar_t)));
    if(code==ERROR_SUCCESS)code=RegSetValueExW(reg,L"ThreadingModel",0,REG_SZ,(const BYTE*)L"Both",10);
    RegCloseKey(reg);HRESULT hr=HRESULT_FROM_WIN32(code);
    // Windows 10 uses the DirectShow video input device; register it alongside
    // the Media Foundation activation class (unused below Windows 11).
    if(SUCCEEDED(hr))hr=PcamDirectShowRegister(module);
    return hr;
}
extern "C" HRESULT __stdcall PcUnregister(){
    std::wstring key=L"SOFTWARE\\Classes\\CLSID\\";key+=PCAM_CLSID;
    auto code=RegDeleteTreeW(HKEY_LOCAL_MACHINE,key.c_str());HRESULT hr=code==ERROR_FILE_NOT_FOUND?S_OK:HRESULT_FROM_WIN32(code);
    HRESULT directShow=PcamDirectShowUnregister();
    return FAILED(hr)?hr:directShow;
}
