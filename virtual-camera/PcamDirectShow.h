// PCam Windows 10 compatibility: DirectShow virtual camera filter.
// Windows 10 has no user-mode Media Foundation virtual-camera API, so the
// same frame file is exposed through a DirectShow video input device.
#pragma once
#include <windows.h>

// Distinct from the Media Foundation activation CLSID (PCAM_CLSID).
inline constexpr GUID PCAM_DSHOW_CLSID = {0x3791d0fd,0x59b4,0x4db9,{0xaf,0x2f,0x28,0x47,0xfc,0xb5,0xcd,0x06}};
inline constexpr wchar_t PCAM_DSHOW_CLSID_STR[] = L"{3791D0FD-59B4-4DB9-AF2F-2847FCB5CD06}";

// Registers/unregisters the in-process filter and its video-input-device
// category entry. Call from the elevated installer (PcRegister). Writes the
// registry view of the calling process: the elevated 64-bit app registers the
// 64-bit view, and regsvr32 (SysWOW64 or System32) registers each bitness.
HRESULT PcamDirectShowRegister(HMODULE module);
HRESULT PcamDirectShowUnregister();

// Same as above, resolving the calling module. Exported as
// DllRegisterServer/DllUnregisterServer for regsvr32.
HRESULT PcamDirectShowRegisterSelf() noexcept;
HRESULT PcamDirectShowUnregisterSelf() noexcept;

// DllGetClassObject dispatch for PCAM_DSHOW_CLSID.
HRESULT PcamDirectShowGetClassObject(REFIID riid, void** result);

// False while a DirectShow filter or class factory from this DLL is alive.
// DllCanUnloadNow must return S_FALSE in that state.
bool PcamDirectShowCanUnload() noexcept;
