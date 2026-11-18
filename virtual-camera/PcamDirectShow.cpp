// PCam Windows 10 compatibility: DirectShow virtual camera filter.
//
// Windows 10 has no user-mode Media Foundation virtual-camera API
// (MFCreateVirtualCamera is Windows 11 22000+). This filter is registered in
// CLSID_VideoInputDeviceCategory and reads the same per-user shared frame file
// the Media Foundation media source uses, so DirectShow-based software can
// select "PCam" on Windows 10 as well.
//
// DirectShow interfaces come from the Windows SDK; no third-party filter code
// is imported. The frame protocol is PCam's own (PcamShared.h).
#include "pch.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

#include <dshow.h>
#include <strmif.h>
// dshow.h already includes uuids.h. That header has no include guard, and
// pch.h defines INITGUID, so a second include would redefine every GUID in
// this translation unit (C2374).

#include "PcamDirectShow.h"

namespace
{
    constexpr wchar_t kVideoInputCategory[] = L"{860BB310-5D01-11D0-BD3B-00A0C911CE86}";
    constexpr LONGLONG kFrameInterval = 333333; // 30 fps in 100 ns units
    constexpr DWORD kFrameBytes = PCAM_STRIDE * PCAM_HEIGHT;
    constexpr DWORD kYuy2Bytes = PCAM_WIDTH * PCAM_HEIGHT * 2;
    constexpr DWORD kRgb32Bytes = PCAM_STRIDE * PCAM_HEIGHT;
    constexpr wchar_t kFramePathKey[] = L"Software\\PCam\\VirtualCamera";
    constexpr wchar_t kFramePathValue[] = L"FramePath";

    // Live objects created from this DLL. DllCanUnloadNow must refuse to unload
    // while a DirectShow filter or class factory still exists.
    std::atomic<LONG> g_objectCount{ 0 };

    // Best-effort diagnostics. The filter runs inside the consuming
    // application (OBS, for example), where PCam's own log cannot see it.
    // Lines go to %TEMP%\PCam-DirectShow.log, capped at 512 KB, and repeated
    // failures are throttled so a black stream does not flood the file.
    void TraceLine(const wchar_t* text) noexcept
    {
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetTempPathW(ARRAYSIZE(path), path);
        if (!length || length >= ARRAYSIZE(path) - 32) return;
        if (FAILED(StringCchCatW(path, ARRAYSIZE(path), L"PCam-DirectShow.log"))) return;

        HANDLE file = CreateFileW(path, FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && size.QuadPart > 512 * 1024)
        {
            LARGE_INTEGER begin{};
            SetFilePointerEx(file, begin, nullptr, FILE_BEGIN);
            SetEndOfFile(file);
        }
        wchar_t line[700]{};
        if (SUCCEEDED(StringCchPrintfW(line, ARRAYSIZE(line), L"[%llu] %s\r\n",
            GetTickCount64(), text)))
        {
            DWORD written = 0;
            WriteFile(file, line, static_cast<DWORD>(wcslen(line) * sizeof(wchar_t)),
                &written, nullptr);
        }
        CloseHandle(file);
    }

    void Trace(const wchar_t* format, ...) noexcept
    {
        wchar_t text[600]{};
        va_list args;
        va_start(args, format);
        const HRESULT hr = StringCchVPrintfW(text, ARRAYSIZE(text), format, args);
        va_end(args);
        if (SUCCEEDED(hr)) TraceLine(text);
    }

    // Throttled variant used from the streaming thread.
    void TraceState(ULONGLONG& next, ULONGLONG interval, const wchar_t* format, ...) noexcept
    {
        const ULONGLONG now = GetTickCount64();
        if (now < next) return;
        next = now + interval;
        wchar_t text[600]{};
        va_list args;
        va_start(args, format);
        const HRESULT hr = StringCchVPrintfW(text, ARRAYSIZE(text), format, args);
        va_end(args);
        if (SUCCEEDED(hr)) TraceLine(text);
    }

    const wchar_t* SubtypeName(REFGUID subtype) noexcept
    {
        if (IsEqualGUID(subtype, MEDIASUBTYPE_YUY2)) return L"YUY2";
        if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB32)) return L"RGB32";
        return L"unsupported";
    }

    // Minimal owning COM pointer. Interfaces are released with Release().
    template <class T>
    class Ptr
    {
    public:
        Ptr() = default;
        ~Ptr() { reset(); }
        Ptr(const Ptr&) = delete;
        Ptr& operator=(const Ptr&) = delete;

        T* get() const noexcept { return m_ptr; }
        T** put() noexcept { reset(); return &m_ptr; }
        T* operator->() const noexcept { return m_ptr; }
        explicit operator bool() const noexcept { return m_ptr != nullptr; }
        void reset(T* value = nullptr) noexcept
        {
            T* old = m_ptr;
            m_ptr = value;
            if (old) old->Release();
        }
        T* detach() noexcept { T* value = m_ptr; m_ptr = nullptr; return value; }
        void attach(T* value) noexcept { reset(value); }

    private:
        T* m_ptr = nullptr;
    };

    void FreeMediaType(AM_MEDIA_TYPE& type) noexcept
    {
        if (type.cbFormat)
        {
            CoTaskMemFree(type.pbFormat);
            type.cbFormat = 0;
            type.pbFormat = nullptr;
        }
        if (type.pUnk)
        {
            type.pUnk->Release();
            type.pUnk = nullptr;
        }
    }

    void DeleteMediaType(AM_MEDIA_TYPE* type) noexcept
    {
        if (type)
        {
            FreeMediaType(*type);
            CoTaskMemFree(type);
        }
    }

    // RAII wrapper for an AM_MEDIA_TYPE filled by CopyMediaType.
    struct MediaTypeHolder
    {
        AM_MEDIA_TYPE value{};
        MediaTypeHolder() = default;
        MediaTypeHolder(const MediaTypeHolder&) = delete;
        MediaTypeHolder& operator=(const MediaTypeHolder&) = delete;
        ~MediaTypeHolder() { FreeMediaType(value); }
    };

    HRESULT CopyMediaType(const AM_MEDIA_TYPE* source, AM_MEDIA_TYPE* target) noexcept
    {
        if (!source || !target) return E_POINTER;
        *target = *source;
        target->pUnk = nullptr;
        target->pbFormat = nullptr;
        target->cbFormat = 0;
        if (source->pUnk)
        {
            source->pUnk->AddRef();
            target->pUnk = source->pUnk;
        }
        if (source->cbFormat)
        {
            target->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(source->cbFormat));
            if (!target->pbFormat)
            {
                if (target->pUnk) { target->pUnk->Release(); target->pUnk = nullptr; }
                return E_OUTOFMEMORY;
            }
            memcpy(target->pbFormat, source->pbFormat, source->cbFormat);
            target->cbFormat = source->cbFormat;
        }
        return S_OK;
    }

    constexpr DWORD FourCC(char a, char b, char c, char d)
    {
        return static_cast<DWORD>(static_cast<BYTE>(a)) |
            (static_cast<DWORD>(static_cast<BYTE>(b)) << 8) |
            (static_cast<DWORD>(static_cast<BYTE>(c)) << 16) |
            (static_cast<DWORD>(static_cast<BYTE>(d)) << 24);
    }

    DWORD BytesPerPixel(REFGUID subtype) noexcept
    {
        if (IsEqualGUID(subtype, MEDIASUBTYPE_YUY2)) return 2;
        return 4;
    }

    HRESULT InitVideoType(REFGUID subtype, AM_MEDIA_TYPE& type) noexcept
    {
        ZeroMemory(&type, sizeof(type));
        type.majortype = MEDIATYPE_Video;
        type.subtype = subtype;
        type.bFixedSizeSamples = TRUE;
        type.bTemporalCompression = FALSE;
        type.formattype = FORMAT_VideoInfo;

        const DWORD bpp = BytesPerPixel(subtype);
        // BytesPerPixel already returns bytes per pixel, so this is the full
        // frame size. Dividing by 8 here once produced a sample one eighth of
        // a frame, and FillSample rejected every buffer as too small.
        type.lSampleSize = PCAM_WIDTH * PCAM_HEIGHT * bpp;

        auto* vih = static_cast<VIDEOINFOHEADER*>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
        if (!vih) return E_OUTOFMEMORY;
        ZeroMemory(vih, sizeof(*vih));
        vih->rcSource = { 0, 0, static_cast<LONG>(PCAM_WIDTH), static_cast<LONG>(PCAM_HEIGHT) };
        vih->rcTarget = vih->rcSource;
        vih->AvgTimePerFrame = kFrameInterval;
        vih->dwBitRate = static_cast<DWORD>(static_cast<ULONGLONG>(type.lSampleSize) * 8 * 30);

        BITMAPINFOHEADER& header = vih->bmiHeader;
        header.biSize = sizeof(BITMAPINFOHEADER);
        header.biWidth = PCAM_WIDTH;
        header.biHeight = PCAM_HEIGHT; // RGB32 is written bottom-up, as DirectShow expects
        header.biPlanes = 1;
        header.biBitCount = static_cast<WORD>(bpp * 8);
        header.biCompression = IsEqualGUID(subtype, MEDIASUBTYPE_YUY2) ? FourCC('Y', 'U', 'Y', '2') : BI_RGB;
        header.biSizeImage = static_cast<DWORD>(type.lSampleSize);

        type.cbFormat = sizeof(VIDEOINFOHEADER);
        type.pbFormat = reinterpret_cast<BYTE*>(vih);
        return S_OK;
    }

    bool IsSupportedType(const AM_MEDIA_TYPE& type) noexcept
    {
        if (!IsEqualGUID(type.majortype, MEDIATYPE_Video)) return false;
        if (!IsEqualGUID(type.subtype, MEDIASUBTYPE_YUY2) && !IsEqualGUID(type.subtype, MEDIASUBTYPE_RGB32)) return false;
        if (!IsEqualGUID(type.formattype, FORMAT_VideoInfo)) return false;
        if (!type.pbFormat || type.cbFormat < sizeof(VIDEOINFOHEADER)) return false;
        const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(type.pbFormat);
        if (vih->bmiHeader.biWidth != static_cast<LONG>(PCAM_WIDTH)) return false;
        if (vih->bmiHeader.biHeight != static_cast<LONG>(PCAM_HEIGHT) &&
            vih->bmiHeader.biHeight != -static_cast<LONG>(PCAM_HEIGHT)) return false;
        return vih->bmiHeader.biBitCount == BytesPerPixel(type.subtype) * 8;
    }

    bool SameFormat(const AM_MEDIA_TYPE& left, const AM_MEDIA_TYPE& right) noexcept
    {
        return IsEqualGUID(left.subtype, right.subtype) && IsSupportedType(left) && IsSupportedType(right);
    }

    void FillStreamCaps(VIDEO_STREAM_CONFIG_CAPS& caps, REFGUID subtype) noexcept
    {
        ZeroMemory(&caps, sizeof(caps));
        caps.guid = FORMAT_VideoInfo;
        caps.VideoStandard = AnalogVideo_None;
        caps.InputSize = { PCAM_WIDTH, PCAM_HEIGHT };
        caps.MinCroppingSize = { PCAM_WIDTH, PCAM_HEIGHT };
        caps.MaxCroppingSize = { PCAM_WIDTH, PCAM_HEIGHT };
        caps.CropGranularityX = 1;
        caps.CropGranularityY = 1;
        caps.CropAlignX = 1;
        caps.CropAlignY = 1;
        caps.MinOutputSize = { PCAM_WIDTH, PCAM_HEIGHT };
        caps.MaxOutputSize = { PCAM_WIDTH, PCAM_HEIGHT };
        caps.OutputGranularityX = 1;
        caps.OutputGranularityY = 1;
        caps.StretchTapsX = 1;
        caps.StretchTapsY = 1;
        caps.ShrinkTapsX = 1;
        caps.ShrinkTapsY = 1;
        caps.MinFrameInterval = kFrameInterval;
        caps.MaxFrameInterval = kFrameInterval;
        const DWORD bpp = BytesPerPixel(subtype);
        caps.MinBitsPerSecond = static_cast<LONG>(static_cast<ULONGLONG>(PCAM_WIDTH) * PCAM_HEIGHT * bpp * 8 * 30);
        caps.MaxBitsPerSecond = caps.MinBitsPerSecond;
    }

    inline BYTE ClampByte(int value) noexcept
    {
        return static_cast<BYTE>(value < 0 ? 0 : (value > 255 ? 255 : value));
    }

    // Registration runs from the elevated helper, which may or may not already
    // have initialized COM on its calling thread.
    struct ComInitializer
    {
        ComInitializer() noexcept : result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
        ~ComInitializer() { if (SUCCEEDED(result)) CoUninitialize(); }
        HRESULT result;
    };

    // Reads the per-user shared frame mapping written by PcStart. Returns the
    // latest BGRA frame, or nullptr when no recent frame is available.
    class FrameReader
    {
    public:
        ~FrameReader() { Close(); }

        const BYTE* Latest() noexcept
        {
            EnsureOpen();
            if (!m_view) { m_valid = false; return nullptr; }

            for (int attempt = 0; attempt < 3; ++attempt)
            {
                LONG sequence = m_view->sequence;
                MemoryBarrier();
                if (sequence & 1) continue;

                const ULONGLONG tick = m_view->tick;
                const LONG slot = m_view->slot;
                if (m_view->magic != PCAM_MAGIC || slot < 0 || slot > 1 || tick == 0 ||
                    GetTickCount64() - tick > 2000)
                {
                    m_valid = false;
                    TraceState(m_nextFailureTrace, 2000,
                        L"no recent frame: magic=0x%08lX, slot=%ld, age=%llu ms",
                        static_cast<unsigned long>(m_view->magic), slot,
                        GetTickCount64() - tick);
                    return nullptr; // publisher stopped: output black like the MF source
                }

                try
                {
                    if (m_frame.size() != kFrameBytes) m_frame.resize(kFrameBytes);
                }
                catch (const std::bad_alloc&) { return m_valid ? m_frame.data() : nullptr; }

                memcpy(m_frame.data(), m_view->pixels[slot], kFrameBytes);
                MemoryBarrier();
                if (sequence == m_view->sequence)
                {
                    m_valid = true;
                    TraceState(m_nextOkTrace, 30000,
                        L"frame ok: slot=%ld, sequence=%ld", slot, sequence);
                    return m_frame.data();
                }
            }
            // A concurrent publisher write can invalidate one snapshot. Keep the
            // previous complete frame briefly instead of flashing black.
            return m_valid ? m_frame.data() : nullptr;
        }

    private:
        void Close() noexcept
        {
            if (m_view) { UnmapViewOfFile(m_view); m_view = nullptr; }
            if (m_mapping) { CloseHandle(m_mapping); m_mapping = nullptr; }
            if (m_file != INVALID_HANDLE_VALUE) { CloseHandle(m_file); m_file = INVALID_HANDLE_VALUE; }
        }

        void EnsureOpen() noexcept
        {
            if (m_view) return;
            const ULONGLONG now = GetTickCount64();
            if (m_lastAttempt && now - m_lastAttempt < 2000) return;
            m_lastAttempt = now;

            wchar_t path[32768]{};
            DWORD size = sizeof(path);
            if (RegGetValueW(HKEY_CURRENT_USER, kFramePathKey, kFramePathValue,
                RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS)
            {
                TraceState(m_nextFailureTrace, 2000,
                    L"frame file: HKCU\\%s\\%s is missing (is PCam running and publishing?)",
                    kFramePathKey, kFramePathValue);
                return;
            }

            m_file = CreateFileW(path, GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (m_file == INVALID_HANDLE_VALUE)
            {
                TraceState(m_nextFailureTrace, 2000, L"frame file open failed (%lu): %s",
                    static_cast<unsigned long>(GetLastError()), path);
                return;
            }

            LARGE_INTEGER length{};
            if (!GetFileSizeEx(m_file, &length) || length.QuadPart < static_cast<LONGLONG>(sizeof(PcamFrames)))
            {
                TraceState(m_nextFailureTrace, 2000, L"frame file too small: %s", path);
                Close();
                return;
            }

            m_mapping = CreateFileMappingW(m_file, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (!m_mapping)
            {
                TraceState(m_nextFailureTrace, 2000, L"frame file mapping failed (%lu): %s",
                    static_cast<unsigned long>(GetLastError()), path);
                Close();
                return;
            }
            m_view = static_cast<const PcamFrames*>(MapViewOfFile(m_mapping, FILE_MAP_READ, 0, 0, sizeof(PcamFrames)));
            if (!m_view)
            {
                TraceState(m_nextFailureTrace, 2000, L"frame file view failed (%lu): %s",
                    static_cast<unsigned long>(GetLastError()), path);
                Close();
                return;
            }
            Trace(L"frame file mapped: %s", path);
        }

        HANDLE m_file = INVALID_HANDLE_VALUE;
        HANDLE m_mapping = nullptr;
        const PcamFrames* m_view = nullptr;
        ULONGLONG m_lastAttempt = 0;
        ULONGLONG m_nextFailureTrace = 0;
        ULONGLONG m_nextOkTrace = 0;
        std::vector<BYTE> m_frame;
        bool m_valid = false;
    };

    class MediaTypeEnumerator final : public IEnumMediaTypes
    {
    public:
        MediaTypeEnumerator() = default;

        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (!ppv) return E_POINTER;
            *ppv = nullptr;
            if (!IsEqualIID(riid, __uuidof(IUnknown)) && !IsEqualIID(riid, __uuidof(IEnumMediaTypes)))
                return E_NOINTERFACE;
            *ppv = static_cast<IEnumMediaTypes*>(this);
            AddRef();
            return S_OK;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return ++m_ref; }
        STDMETHODIMP_(ULONG) Release() override
        {
            const ULONG count = --m_ref;
            if (!count) delete this;
            return count;
        }

        STDMETHODIMP Next(ULONG count, AM_MEDIA_TYPE** types, ULONG* fetched) override
        {
            if (!types) return E_POINTER;
            ULONG copied = 0;
            while (copied < count && m_index < 2)
            {
                const GUID subtype = m_index == 0 ? MEDIASUBTYPE_YUY2 : MEDIASUBTYPE_RGB32;
                auto* type = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
                if (!type)
                {
                    for (ULONG i = 0; i < copied; ++i) DeleteMediaType(types[i]);
                    if (fetched) *fetched = 0;
                    return E_OUTOFMEMORY;
                }
                ZeroMemory(type, sizeof(*type));
                if (FAILED(InitVideoType(subtype, *type)))
                {
                    CoTaskMemFree(type);
                    for (ULONG i = 0; i < copied; ++i) DeleteMediaType(types[i]);
                    if (fetched) *fetched = 0;
                    return E_OUTOFMEMORY;
                }
                types[copied++] = type;
                ++m_index;
            }
            if (fetched) *fetched = copied;
            return copied == count ? S_OK : S_FALSE;
        }

        STDMETHODIMP Skip(ULONG count) override
        {
            if (count > 2 - m_index) { m_index = 2; return S_FALSE; }
            m_index += count;
            return S_OK;
        }

        STDMETHODIMP Reset() override { m_index = 0; return S_OK; }

        STDMETHODIMP Clone(IEnumMediaTypes** enumerator) override
        {
            if (!enumerator) return E_POINTER;
            *enumerator = nullptr;
            auto* copy = new (std::nothrow) MediaTypeEnumerator();
            if (!copy) return E_OUTOFMEMORY;
            copy->m_index = m_index;
            *enumerator = copy;
            return S_OK;
        }

    private:
        std::atomic<ULONG> m_ref{ 1 };
        ULONG m_index = 0;
    };

    class PinEnumerator final : public IEnumPins
    {
    public:
        explicit PinEnumerator(IPin* pin) : m_pin(pin)
        {
            if (m_pin) m_pin->AddRef();
        }
        ~PinEnumerator()
        {
            if (m_pin) m_pin->Release();
        }

        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (!ppv) return E_POINTER;
            *ppv = nullptr;
            if (!IsEqualIID(riid, __uuidof(IUnknown)) && !IsEqualIID(riid, __uuidof(IEnumPins)))
                return E_NOINTERFACE;
            *ppv = static_cast<IEnumPins*>(this);
            AddRef();
            return S_OK;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return ++m_ref; }
        STDMETHODIMP_(ULONG) Release() override
        {
            const ULONG count = --m_ref;
            if (!count) delete this;
            return count;
        }

        STDMETHODIMP Next(ULONG count, IPin** pins, ULONG* fetched) override
        {
            if (!pins) return E_POINTER;
            ULONG copied = 0;
            if (count && !m_index && m_pin)
            {
                m_pin->AddRef();
                pins[0] = m_pin;
                m_index = 1;
                copied = 1;
            }
            if (fetched) *fetched = copied;
            return copied == count ? S_OK : S_FALSE;
        }

        STDMETHODIMP Skip(ULONG count) override
        {
            if (!count) return S_OK;
            if (!m_index) { m_index = 1; return count == 1 ? S_OK : S_FALSE; }
            return S_FALSE;
        }

        STDMETHODIMP Reset() override { m_index = 0; return S_OK; }

        STDMETHODIMP Clone(IEnumPins** enumerator) override
        {
            if (!enumerator) return E_POINTER;
            *enumerator = nullptr;
            auto* copy = new (std::nothrow) PinEnumerator(m_pin);
            if (!copy) return E_OUTOFMEMORY;
            copy->m_index = m_index;
            *enumerator = copy;
            return S_OK;
        }

    private:
        std::atomic<ULONG> m_ref{ 1 };
        IPin* m_pin = nullptr;
        ULONG m_index = 0;
    };

    class PcamFilter;

    class PcamOutputPin final : public IPin, public IAMStreamConfig, public IKsPropertySet
    {
    public:
        explicit PcamOutputPin(PcamFilter* filter) noexcept;
        ~PcamOutputPin();

        void DetachFilter() noexcept;
        HRESULT StartStreaming() noexcept;
        void StopStreaming() noexcept;
        bool IsStreaming() const noexcept { return m_streaming; }
        void SetClock(IReferenceClock* clock) noexcept;
        IReferenceClock* Clock() noexcept;

        // IUnknown
        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
        STDMETHODIMP_(ULONG) AddRef() override;
        STDMETHODIMP_(ULONG) Release() override;

        // IPin
        STDMETHODIMP Connect(IPin* receivePin, const AM_MEDIA_TYPE* mediaType) override;
        STDMETHODIMP ReceiveConnection(IPin* connector, const AM_MEDIA_TYPE* mediaType) override;
        STDMETHODIMP Disconnect() override;
        STDMETHODIMP ConnectedTo(IPin** pin) override;
        STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* mediaType) override;
        STDMETHODIMP QueryPinInfo(PIN_INFO* info) override;
        STDMETHODIMP QueryDirection(PIN_DIRECTION* direction) override;
        STDMETHODIMP QueryId(LPWSTR* id) override;
        STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* mediaType) override;
        STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** enumerator) override;
        STDMETHODIMP QueryInternalConnections(IPin** pins, ULONG* count) override;
        STDMETHODIMP EndOfStream() override;
        STDMETHODIMP BeginFlush() override;
        STDMETHODIMP EndFlush() override;
        STDMETHODIMP NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate) override;

        // IAMStreamConfig
        STDMETHODIMP SetFormat(AM_MEDIA_TYPE* mediaType) override;
        STDMETHODIMP GetFormat(AM_MEDIA_TYPE** mediaType) override;
        STDMETHODIMP GetNumberOfCapabilities(int* count, int* size) override;
        STDMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE** mediaType, BYTE* caps) override;

        // IKsPropertySet
        STDMETHODIMP Set(REFGUID propertySet, DWORD id, LPVOID instanceData, DWORD instanceLength,
            LPVOID propertyData, DWORD dataLength) override;
        STDMETHODIMP Get(REFGUID propertySet, DWORD id, LPVOID instanceData, DWORD instanceLength,
            LPVOID propertyData, DWORD dataLength, DWORD* returned) override;
        STDMETHODIMP QuerySupported(REFGUID propertySet, DWORD id, DWORD* support) override;

    private:
        HRESULT DecideAllocator(IMemInputPin* input, IMemAllocator** allocator, LONG sampleSize) noexcept;
        void StreamingLoop() noexcept;
        HRESULT FillSample(IMediaSample* sample) noexcept;
        LONGLONG Now() noexcept;

        std::atomic<ULONG> m_ref{ 1 };
        PcamFilter* m_filter = nullptr;

        mutable std::mutex m_lock;
        AM_MEDIA_TYPE m_mediaType{};
        bool m_mediaTypeValid = false;
        IMemInputPin* m_input = nullptr;
        IMemAllocator* m_allocator = nullptr;
        IReferenceClock* m_clock = nullptr;
        bool m_connected = false;

        std::mutex m_runLock;
        std::condition_variable m_runCv;
        std::thread m_thread;
        bool m_stop = false;
        std::atomic<bool> m_streaming{ false };
        FrameReader m_frames;
    };

    class PcamFilter final : public IBaseFilter, public IAMFilterMiscFlags
    {
    public:
        PcamFilter();
        ~PcamFilter();

        // IUnknown
        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
        STDMETHODIMP_(ULONG) AddRef() override;
        STDMETHODIMP_(ULONG) Release() override;

        // IPersist
        STDMETHODIMP GetClassID(CLSID* clsid) override;

        // IMediaFilter
        STDMETHODIMP Stop() override;
        STDMETHODIMP Pause() override;
        STDMETHODIMP Run(REFERENCE_TIME start) override;
        STDMETHODIMP GetState(DWORD timeout, FILTER_STATE* state) override;
        STDMETHODIMP SetSyncSource(IReferenceClock* clock) override;
        STDMETHODIMP GetSyncSource(IReferenceClock** clock) override;

        // IBaseFilter
        STDMETHODIMP EnumPins(IEnumPins** enumerator) override;
        STDMETHODIMP FindPin(LPCWSTR id, IPin** pin) override;
        STDMETHODIMP QueryFilterInfo(FILTER_INFO* info) override;
        STDMETHODIMP JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) override;
        STDMETHODIMP QueryVendorInfo(LPWSTR* vendor) override;

        // IAMFilterMiscFlags
        STDMETHODIMP_(ULONG) GetMiscFlags() override;

    private:
        std::atomic<ULONG> m_ref{ 1 };
        std::mutex m_lock;
        std::atomic<FILTER_STATE> m_state{ State_Stopped };
        IFilterGraph* m_graph = nullptr;
        wchar_t m_name[128]{};
        PcamOutputPin* m_pin = nullptr;
    };

    //////////////////////////////////////////////////////////////////////////
    // PcamOutputPin

    PcamOutputPin::PcamOutputPin(PcamFilter* filter) noexcept : m_filter(filter)
    {
        m_mediaTypeValid = SUCCEEDED(InitVideoType(MEDIASUBTYPE_YUY2, m_mediaType));
        ++g_objectCount;
    }

    PcamOutputPin::~PcamOutputPin()
    {
        StopStreaming();
        if (m_input) { m_input->Release(); m_input = nullptr; }
        if (m_allocator) { m_allocator->Release(); m_allocator = nullptr; }
        if (m_clock) { m_clock->Release(); m_clock = nullptr; }
        FreeMediaType(m_mediaType);
        --g_objectCount;
    }

    void PcamOutputPin::SetClock(IReferenceClock* clock) noexcept
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (clock) clock->AddRef();
        if (m_clock) m_clock->Release();
        m_clock = clock;
    }

    IReferenceClock* PcamOutputPin::Clock() noexcept
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (m_clock) m_clock->AddRef();
        return m_clock;
    }

    void PcamOutputPin::DetachFilter() noexcept
    {
        StopStreaming();
        std::lock_guard<std::mutex> lock(m_lock);
        m_filter = nullptr;
    }

    STDMETHODIMP PcamOutputPin::QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IPin)))
            *ppv = static_cast<IPin*>(this);
        else if (IsEqualIID(riid, __uuidof(IAMStreamConfig)))
            *ppv = static_cast<IAMStreamConfig*>(this);
        else if (IsEqualIID(riid, __uuidof(IKsPropertySet)))
            *ppv = static_cast<IKsPropertySet*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG) PcamOutputPin::AddRef()
    {
        return ++m_ref;
    }

    STDMETHODIMP_(ULONG) PcamOutputPin::Release()
    {
        const ULONG count = --m_ref;
        if (!count) delete this;
        return count;
    }

    STDMETHODIMP PcamOutputPin::Connect(IPin* receivePin, const AM_MEDIA_TYPE* mediaType)
    {
        if (!receivePin) return E_POINTER;

        PIN_DIRECTION direction = PINDIR_OUTPUT;
        RETURN_IF_FAILED(receivePin->QueryDirection(&direction));
        if (direction != PINDIR_INPUT) return VFW_E_INVALID_DIRECTION;

        AM_MEDIA_TYPE type{};
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (m_connected) return VFW_E_ALREADY_CONNECTED;
            RETURN_IF_FAILED(CopyMediaType(mediaType ? mediaType : &m_mediaType, &type));
        }
        if (!IsSupportedType(type))
        {
            Trace(L"connect rejected: majortype=%08lX, subtype=%08lX",
                static_cast<unsigned long>(type.majortype.Data1),
                static_cast<unsigned long>(type.subtype.Data1));
            FreeMediaType(type);
            return VFW_E_TYPE_NOT_ACCEPTED;
        }
        // Use our canonical format (stride, sample size, row order) for the
        // connection: the receiver must see the same layout FillSample writes.
        {
            const GUID subtype = type.subtype;
            FreeMediaType(type);
            RETURN_IF_FAILED(InitVideoType(subtype, type));
        }
        // Inform the input pin of the connection before negotiating buffers;
        // input pins validate the media type in ReceiveConnection.
        HRESULT hr = receivePin->ReceiveConnection(static_cast<IPin*>(this), &type);
        if (FAILED(hr))
        {
            Trace(L"connect: ReceiveConnection(%s) failed 0x%08lX",
                SubtypeName(type.subtype), static_cast<unsigned long>(hr));
            FreeMediaType(type);
            return hr;
        }

        Ptr<IMemInputPin> input;
        hr = receivePin->QueryInterface(__uuidof(IMemInputPin), reinterpret_cast<void**>(input.put()));
        if (FAILED(hr))
        {
            Trace(L"connect: IMemInputPin unavailable 0x%08lX", static_cast<unsigned long>(hr));
            receivePin->Disconnect();
            FreeMediaType(type);
            return hr;
        }

        Ptr<IMemAllocator> allocator;
        hr = DecideAllocator(input.get(), allocator.put(), type.lSampleSize);
        if (FAILED(hr))
        {
            Trace(L"connect: allocator failed 0x%08lX", static_cast<unsigned long>(hr));
            receivePin->Disconnect();
            FreeMediaType(type);
            return hr;
        }

        bool alreadyConnected = false;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (m_connected)
            {
                alreadyConnected = true;
            }
            else
            {
                FreeMediaType(m_mediaType);
                m_mediaType = type;
                m_mediaTypeValid = true;
                m_input = input.detach();
                m_allocator = allocator.detach();
                m_connected = true;
            }
        }
        if (alreadyConnected)
        {
            // Disconnect outside the lock: the receiving pin may call back.
            receivePin->Disconnect();
            FreeMediaType(type);
            return VFW_E_ALREADY_CONNECTED;
        }
        Trace(L"connect: %s, sample=%lu bytes", SubtypeName(type.subtype), type.lSampleSize);
        return S_OK;
    }

    STDMETHODIMP PcamOutputPin::ReceiveConnection(IPin*, const AM_MEDIA_TYPE*)
    {
        return E_UNEXPECTED; // output pins never accept incoming connections
    }

    STDMETHODIMP PcamOutputPin::Disconnect()
    {
        IMemInputPin* input = nullptr;
        IMemAllocator* allocator = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            input = m_input;
            allocator = m_allocator;
            m_input = nullptr;
            m_allocator = nullptr;
            m_connected = false;
        }
        if (allocator) allocator->Release();
        if (input) input->Release();
        Trace(L"disconnect");
        return S_OK;
    }

    STDMETHODIMP PcamOutputPin::ConnectedTo(IPin** pin)
    {
        if (!pin) return E_POINTER;
        *pin = nullptr;
        std::lock_guard<std::mutex> lock(m_lock);
        if (!m_connected || !m_input) return VFW_E_NOT_CONNECTED;
        return m_input->QueryInterface(__uuidof(IPin), reinterpret_cast<void**>(pin));
    }

    STDMETHODIMP PcamOutputPin::ConnectionMediaType(AM_MEDIA_TYPE* mediaType)
    {
        if (!mediaType) return E_POINTER;
        std::lock_guard<std::mutex> lock(m_lock);
        if (!m_connected) return VFW_E_NOT_CONNECTED;
        return CopyMediaType(&m_mediaType, mediaType);
    }

    STDMETHODIMP PcamOutputPin::QueryPinInfo(PIN_INFO* info)
    {
        if (!info) return E_POINTER;
        std::lock_guard<std::mutex> lock(m_lock);
        info->pFilter = static_cast<IBaseFilter*>(m_filter);
        if (info->pFilter) info->pFilter->AddRef();
        info->dir = PINDIR_OUTPUT;
        StringCchCopyW(info->achName, ARRAYSIZE(info->achName), L"Output");
        return S_OK;
    }

    STDMETHODIMP PcamOutputPin::QueryDirection(PIN_DIRECTION* direction)
    {
        if (!direction) return E_POINTER;
        *direction = PINDIR_OUTPUT;
        return S_OK;
    }

    STDMETHODIMP PcamOutputPin::QueryId(LPWSTR* id)
    {
        if (!id) return E_POINTER;
        *id = static_cast<LPWSTR>(CoTaskMemAlloc(7 * sizeof(wchar_t)));
        if (!*id) return E_OUTOFMEMORY;
        memcpy(*id, L"Output", 7 * sizeof(wchar_t));
        return S_OK;
    }

    STDMETHODIMP PcamOutputPin::QueryAccept(const AM_MEDIA_TYPE* mediaType)
    {
        if (!mediaType) return E_POINTER;
        return IsSupportedType(*mediaType) ? S_OK : S_FALSE;
    }

    STDMETHODIMP PcamOutputPin::EnumMediaTypes(IEnumMediaTypes** enumerator)
    {
        if (!enumerator) return E_POINTER;
        *enumerator = new (std::nothrow) MediaTypeEnumerator();
        return *enumerator ? S_OK : E_OUTOFMEMORY;
    }

    STDMETHODIMP PcamOutputPin::QueryInternalConnections(IPin**, ULONG* count)
    {
        if (count) *count = 0;
        return E_NOTIMPL;
    }

    STDMETHODIMP PcamOutputPin::EndOfStream() { return S_OK; }
    STDMETHODIMP PcamOutputPin::BeginFlush() { return S_OK; }
    STDMETHODIMP PcamOutputPin::EndFlush() { return S_OK; }
    STDMETHODIMP PcamOutputPin::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) { return S_OK; }

    HRESULT PcamOutputPin::DecideAllocator(IMemInputPin* input, IMemAllocator** allocator, LONG sampleSize) noexcept
    {
        *allocator = nullptr;

        Ptr<IMemAllocator> memory;
        HRESULT hr = input->GetAllocator(memory.put());
        if (hr != S_OK || !memory)
        {
            memory.reset();
            hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER,
                __uuidof(IMemAllocator), reinterpret_cast<void**>(memory.put()));
            RETURN_IF_FAILED(hr);
        }

        ALLOCATOR_PROPERTIES requested{};
        requested.cBuffers = 3;
        requested.cbBuffer = sampleSize;
        requested.cbAlign = 1;
        requested.cbPrefix = 0;
        ALLOCATOR_PROPERTIES actual{};
        RETURN_IF_FAILED(memory->SetProperties(&requested, &actual));
        if (actual.cbBuffer < requested.cbBuffer) return VFW_E_BUFFER_OVERFLOW;
        RETURN_IF_FAILED(memory->Commit());
        RETURN_IF_FAILED(input->NotifyAllocator(memory.get(), FALSE));

        *allocator = memory.detach();
        return S_OK;
    }

    STDMETHODIMP PcamOutputPin::SetFormat(AM_MEDIA_TYPE* mediaType)
    {
        if (!mediaType) return E_POINTER;
        if (!IsSupportedType(*mediaType))
        {
            Trace(L"SetFormat rejected: subtype=%08lX",
                static_cast<unsigned long>(mediaType->subtype.Data1));
            return VFW_E_INVALIDMEDIATYPE;
        }

        const GUID subtype = mediaType->subtype;
        std::lock_guard<std::mutex> lock(m_lock);
        if (m_connected)
        {
            return SameFormat(m_mediaType, *mediaType) ? S_OK : VFW_E_ALREADY_CONNECTED;
        }
        AM_MEDIA_TYPE copy{};
        RETURN_IF_FAILED(InitVideoType(subtype, copy));
        FreeMediaType(m_mediaType);
        m_mediaType = copy;
        m_mediaTypeValid = true;
        Trace(L"SetFormat: %s, sample=%lu", SubtypeName(subtype), copy.lSampleSize);
        return S_OK;
    }

    STDMETHODIMP PcamOutputPin::GetFormat(AM_MEDIA_TYPE** mediaType)
    {
        if (!mediaType) return E_POINTER;
        *mediaType = nullptr;
        std::lock_guard<std::mutex> lock(m_lock);
        if (!m_mediaTypeValid) return E_UNEXPECTED;
        auto* copy = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
        if (!copy) return E_OUTOFMEMORY;
        if (FAILED(CopyMediaType(&m_mediaType, copy)))
        {
            CoTaskMemFree(copy);
            return E_OUTOFMEMORY;
        }
        *mediaType = copy;
        return S_OK;
    }

    STDMETHODIMP PcamOutputPin::GetNumberOfCapabilities(int* count, int* size)
    {
        if (!count || !size) return E_POINTER;
        *count = 2;
        *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
        return S_OK;
    }

    STDMETHODIMP PcamOutputPin::GetStreamCaps(int index, AM_MEDIA_TYPE** mediaType, BYTE* caps)
    {
        if (!mediaType || !caps) return E_POINTER;
        if (index < 0 || index > 1) return E_INVALIDARG;
        *mediaType = nullptr;

        const GUID subtype = index == 0 ? MEDIASUBTYPE_YUY2 : MEDIASUBTYPE_RGB32;
        auto* type = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
        if (!type) return E_OUTOFMEMORY;
        const HRESULT hr = InitVideoType(subtype, *type);
        if (FAILED(hr))
        {
            CoTaskMemFree(type);
            return hr;
        }
        VIDEO_STREAM_CONFIG_CAPS streamCaps{};
        FillStreamCaps(streamCaps, subtype);
        memcpy(caps, &streamCaps, sizeof(streamCaps));
        *mediaType = type;
        return S_OK;
    }

    STDMETHODIMP PcamOutputPin::Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD)
    {
        return E_NOTIMPL;
    }

    STDMETHODIMP PcamOutputPin::Get(REFGUID propertySet, DWORD id, LPVOID, DWORD,
        LPVOID propertyData, DWORD dataLength, DWORD* returned)
    {
        if (!IsEqualGUID(propertySet, AMPROPSETID_Pin)) return E_PROP_SET_UNSUPPORTED;
        if (id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
        if (!propertyData || dataLength < sizeof(GUID)) return E_UNEXPECTED;
        *static_cast<GUID*>(propertyData) = PIN_CATEGORY_CAPTURE;
        if (returned) *returned = sizeof(GUID);
        return S_OK;
    }

    STDMETHODIMP PcamOutputPin::QuerySupported(REFGUID propertySet, DWORD id, DWORD* support)
    {
        if (!IsEqualGUID(propertySet, AMPROPSETID_Pin)) return E_PROP_SET_UNSUPPORTED;
        if (id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
        if (support) *support = KSPROPERTY_SUPPORT_GET;
        return S_OK;
    }

    HRESULT PcamOutputPin::StartStreaming() noexcept
    {
        std::thread finished;
        {
            std::lock_guard<std::mutex> lock(m_runLock);
            if (m_thread.joinable())
            {
                if (m_streaming) return S_OK;
                // The thread ended on its own (for example after a disconnect);
                // reclaim it before starting a fresh one.
                finished = std::move(m_thread);
            }
            m_stop = false;
            m_streaming = true;
            try
            {
                m_thread = std::thread([this]() { StreamingLoop(); });
            }
            catch (...)
            {
                m_streaming = false;
                m_stop = true;
                return E_FAIL;
            }
        }
        if (finished.joinable()) finished.join();
        Trace(L"StartStreaming: capture thread started");
        return S_OK;
    }

    void PcamOutputPin::StopStreaming() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(m_runLock);
            if (!m_thread.joinable()) return;
            m_stop = true;
        }
        m_runCv.notify_all();
        if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id())
            m_thread.join();
        Trace(L"StopStreaming: capture thread joined");
    }

    HRESULT PcamOutputPin::FillSample(IMediaSample* sample) noexcept
    {
        BYTE* destination = nullptr;
        RETURN_IF_FAILED(sample->GetPointer(&destination));
        const LONG capacity = sample->GetSize();

        GUID subtype = MEDIASUBTYPE_YUY2;
        bool rgbTopDown = false;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (!m_mediaTypeValid) return E_UNEXPECTED;
            subtype = m_mediaType.subtype;
            if (m_mediaType.pbFormat && m_mediaType.cbFormat >= sizeof(VIDEOINFOHEADER))
            {
                const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(m_mediaType.pbFormat);
                rgbTopDown = vih->bmiHeader.biHeight < 0;
            }
        }

        const DWORD needed = IsEqualGUID(subtype, MEDIASUBTYPE_YUY2) ? kYuy2Bytes : kRgb32Bytes;
        if (!destination || capacity < static_cast<LONG>(needed)) return VFW_E_BUFFER_OVERFLOW;

        const BYTE* frame = m_frames.Latest();
        if (!frame)
        {
            if (IsEqualGUID(subtype, MEDIASUBTYPE_YUY2))
            {
                auto* words = reinterpret_cast<DWORD*>(destination);
                for (DWORD i = 0; i < needed / 4; ++i) words[i] = 0x80108010; // Y=16, U=V=128
            }
            else
            {
                ZeroMemory(destination, needed);
            }
        }
        else if (IsEqualGUID(subtype, MEDIASUBTYPE_YUY2))
        {
            // YUY2 declares a positive biHeight, so rows go bottom-up just like
            // the RGB32 path.
            for (DWORD y = 0; y < PCAM_HEIGHT; ++y)
            {
                const BYTE* source = frame + static_cast<size_t>(y) * PCAM_STRIDE;
                BYTE* target = destination +
                    static_cast<size_t>(PCAM_HEIGHT - 1 - y) * PCAM_WIDTH * 2;
                for (DWORD x = 0; x < PCAM_WIDTH; x += 2)
                {
                    const int b0 = source[0], g0 = source[1], r0 = source[2];
                    const int b1 = source[4], g1 = source[5], r1 = source[6];
                    const int y0 = ((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8) + 16;
                    const int y1 = ((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16;
                    const int u = (((-38 * r0 - 74 * g0 + 112 * b0 + 128) >> 8) +
                        ((-38 * r1 - 74 * g1 + 112 * b1 + 128) >> 8) + 2) / 2 + 128;
                    const int v = (((112 * r0 - 94 * g0 - 18 * b0 + 128) >> 8) +
                        ((112 * r1 - 94 * g1 - 18 * b1 + 128) >> 8) + 2) / 2 + 128;
                    target[0] = ClampByte(y0);
                    target[1] = ClampByte(u);
                    target[2] = ClampByte(y1);
                    target[3] = ClampByte(v);
                    target += 4;
                    source += 8;
                }
            }
        }
        else
        {
            // DirectShow RGB32 is bottom-up unless biHeight is negative.
            if (rgbTopDown)
            {
                memcpy(destination, frame, needed);
            }
            else
            {
                for (DWORD y = 0; y < PCAM_HEIGHT; ++y)
                {
                    memcpy(destination + static_cast<size_t>(PCAM_HEIGHT - 1 - y) * PCAM_STRIDE,
                        frame + static_cast<size_t>(y) * PCAM_STRIDE, PCAM_STRIDE);
                }
            }
        }

        return sample->SetActualDataLength(static_cast<LONG>(needed));
    }

    LONGLONG PcamOutputPin::Now() noexcept
    {
        IReferenceClock* clock = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (m_clock) { m_clock->AddRef(); clock = m_clock; }
        }
        if (clock)
        {
            LONGLONG value = 0;
            const HRESULT hr = clock->GetTime(&value);
            clock->Release();
            if (SUCCEEDED(hr)) return value;
        }
        return (LONGLONG)GetTickCount64() * 10000;
    }

    void PcamOutputPin::StreamingLoop() noexcept
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        using Clock = std::chrono::steady_clock;
        LONGLONG base = 0;
        bool baseSet = false;
        ULONGLONG nextTrace = 0;
        bool traceDelivered = false;
        auto next = Clock::now();
        m_streaming = true;

        // The thread lives until StopStreaming sets m_stop. Per-iteration
        // failures (disconnect, allocator starvation, exceptions) must not end
        // it, otherwise a later Run could observe a stale "streaming" flag with
        // no thread behind it.
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lock(m_runLock);
                if (m_stop) break;
            }

            bool connected = false;
            {
                std::lock_guard<std::mutex> guard(m_lock);
                connected = m_connected;
            }
            if (!connected)
            {
                std::unique_lock<std::mutex> lock(m_runLock);
                m_runCv.wait_for(lock, std::chrono::milliseconds(50), [this]() { return m_stop; });
                continue;
            }

            try
            {
                Ptr<IMemInputPin> input;
                Ptr<IMemAllocator> allocator;
                MediaTypeHolder mediaType;
                bool copied = false;
                {
                    std::lock_guard<std::mutex> lock(m_lock);
                    if (!m_connected) continue;
                    if (m_input) { m_input->AddRef(); input.attach(m_input); }
                    if (m_allocator) { m_allocator->AddRef(); allocator.attach(m_allocator); }
                    copied = SUCCEEDED(CopyMediaType(&m_mediaType, &mediaType.value));
                }
                if (!copied || !input || !allocator)
                {
                    TraceState(nextTrace, 1000,
                        L"stream: negotiated state unusable (mediaType=%d, input=%d, allocator=%d)",
                        copied ? 1 : 0, input ? 1 : 0, allocator ? 1 : 0);
                    // Do not spin if the negotiated state is momentarily unusable.
                    std::unique_lock<std::mutex> lock(m_runLock);
                    m_runCv.wait_for(lock, std::chrono::milliseconds(1),
                        [this]() { return m_stop; });
                    continue;
                }

                Ptr<IMediaSample> sample;
                HRESULT hr = allocator->GetBuffer(sample.put(), nullptr, nullptr, 200);
                if (FAILED(hr))
                {
                    TraceState(nextTrace, 1000, L"stream: GetBuffer failed 0x%08lX",
                        static_cast<unsigned long>(hr));
                }
                else if (sample)
                {
                    const LONGLONG now = Now();
                    if (!baseSet) { base = now; baseSet = true; }
                    REFERENCE_TIME start = now - base;
                    REFERENCE_TIME stop = start + kFrameInterval;

                    HRESULT filled = FillSample(sample.get());
                    if (FAILED(filled))
                    {
                        TraceState(nextTrace, 1000, L"stream: FillSample failed 0x%08lX",
                            static_cast<unsigned long>(filled));
                    }
                    else
                    {
                        sample->SetTime(&start, &stop);
                        sample->SetSyncPoint(TRUE);
                        sample->SetDiscontinuity(FALSE);
                        sample->SetPreroll(FALSE);
                        sample->SetMediaType(&mediaType.value);
                        hr = input->Receive(sample.get());
                        if (FAILED(hr))
                        {
                            TraceState(nextTrace, 1000, L"stream: Receive failed 0x%08lX",
                                static_cast<unsigned long>(hr));
                        }
                        else if (!traceDelivered)
                        {
                            traceDelivered = true;
                            Trace(L"stream: first sample delivered (t=%lld)", static_cast<long long>(start));
                        }
                    }
                }

                if (hr == S_FALSE)
                {
                    std::unique_lock<std::mutex> lock(m_runLock);
                    m_runCv.wait_for(lock, std::chrono::milliseconds(5),
                        [this]() { return m_stop; });
                    continue;
                }
            }
            catch (...)
            {
                // Keep the thread alive; the pacing wait below still applies.
            }

            next += std::chrono::microseconds(33333);
            const auto current = Clock::now();
            if (current >= next)
            {
                next = current;
            }
            else
            {
                std::unique_lock<std::mutex> lock(m_runLock);
                m_runCv.wait_until(lock, next, [this]() { return m_stop; });
            }
        }
        if (SUCCEEDED(com)) CoUninitialize();
        m_streaming = false;
    }

    //////////////////////////////////////////////////////////////////////////
    // PcamFilter

    PcamFilter::PcamFilter()
    {
        m_pin = new PcamOutputPin(this);
        StringCchCopyW(m_name, ARRAYSIZE(m_name), L"PCam");
        ++g_objectCount;
    }

    PcamFilter::~PcamFilter()
    {
        if (m_pin)
        {
            m_pin->DetachFilter();
            m_pin->Release();
            m_pin = nullptr;
        }
        --g_objectCount;
    }

    STDMETHODIMP PcamFilter::QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IPersist)) ||
            IsEqualIID(riid, __uuidof(IMediaFilter)) || IsEqualIID(riid, __uuidof(IBaseFilter)))
            *ppv = static_cast<IBaseFilter*>(this);
        else if (IsEqualIID(riid, __uuidof(IAMFilterMiscFlags)))
            *ppv = static_cast<IAMFilterMiscFlags*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG) PcamFilter::AddRef()
    {
        return ++m_ref;
    }

    STDMETHODIMP_(ULONG) PcamFilter::Release()
    {
        const ULONG count = --m_ref;
        if (!count) delete this;
        return count;
    }

    STDMETHODIMP PcamFilter::GetClassID(CLSID* clsid)
    {
        if (!clsid) return E_POINTER;
        *clsid = PCAM_DSHOW_CLSID;
        return S_OK;
    }

    STDMETHODIMP PcamFilter::Stop()
    {
        Trace(L"filter: Stop");
        m_state = State_Stopped;
        if (m_pin) m_pin->StopStreaming();
        return S_OK;
    }

    STDMETHODIMP PcamFilter::Pause()
    {
        Trace(L"filter: Pause");
        m_state = State_Paused;
        return m_pin ? m_pin->StartStreaming() : E_UNEXPECTED;
    }

    STDMETHODIMP PcamFilter::Run(REFERENCE_TIME)
    {
        Trace(L"filter: Run");
        m_state = State_Running;
        return m_pin ? m_pin->StartStreaming() : E_UNEXPECTED;
    }

    STDMETHODIMP PcamFilter::GetState(DWORD, FILTER_STATE* state)
    {
        if (!state) return E_POINTER;
        *state = m_state;
        if (*state == State_Paused && m_pin && m_pin->IsStreaming()) return VFW_S_CANT_CUE;
        return S_OK;
    }

    STDMETHODIMP PcamFilter::SetSyncSource(IReferenceClock* clock)
    {
        if (!m_pin) return E_UNEXPECTED;
        m_pin->SetClock(clock);
        return S_OK;
    }

    STDMETHODIMP PcamFilter::GetSyncSource(IReferenceClock** clock)
    {
        if (!clock) return E_POINTER;
        *clock = m_pin ? m_pin->Clock() : nullptr;
        return S_OK;
    }

    STDMETHODIMP PcamFilter::EnumPins(IEnumPins** enumerator)
    {
        if (!enumerator) return E_POINTER;
        *enumerator = nullptr;
        if (!m_pin) return E_UNEXPECTED;
        auto* pins = new (std::nothrow) PinEnumerator(m_pin);
        if (!pins) return E_OUTOFMEMORY;
        *enumerator = pins;
        return S_OK;
    }

    STDMETHODIMP PcamFilter::FindPin(LPCWSTR id, IPin** pin)
    {
        if (!id || !pin) return E_POINTER;
        *pin = nullptr;
        if (lstrcmpiW(id, L"Output") != 0) return VFW_E_NOT_FOUND;
        if (!m_pin) return E_UNEXPECTED;
        m_pin->AddRef();
        *pin = m_pin;
        return S_OK;
    }

    STDMETHODIMP PcamFilter::QueryFilterInfo(FILTER_INFO* info)
    {
        if (!info) return E_POINTER;
        IFilterGraph* graph = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            StringCchCopyW(info->achName, ARRAYSIZE(info->achName), m_name);
            graph = m_graph;
        }
        info->pGraph = graph;
        if (graph) graph->AddRef();
        return S_OK;
    }

    STDMETHODIMP PcamFilter::JoinFilterGraph(IFilterGraph* graph, LPCWSTR name)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_graph = graph;
        if (name) StringCchCopyW(m_name, ARRAYSIZE(m_name), name);
        return S_OK;
    }

    STDMETHODIMP PcamFilter::QueryVendorInfo(LPWSTR*)
    {
        return E_NOTIMPL;
    }

    STDMETHODIMP_(ULONG) PcamFilter::GetMiscFlags()
    {
        return AM_FILTER_MISC_FLAGS_IS_SOURCE;
    }

    //////////////////////////////////////////////////////////////////////////
    // Factory

    class PcamClassFactory final : public IClassFactory
    {
    public:
        PcamClassFactory() noexcept { ++g_objectCount; }
        ~PcamClassFactory() { --g_objectCount; }

        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (!ppv) return E_POINTER;
            *ppv = nullptr;
            if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IClassFactory)))
                *ppv = static_cast<IClassFactory*>(this);
            else
                return E_NOINTERFACE;
            AddRef();
            return S_OK;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return ++m_ref; }
        STDMETHODIMP_(ULONG) Release() override
        {
            const ULONG count = --m_ref;
            if (!count) delete this;
            return count;
        }
        STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
        {
            if (outer) return CLASS_E_NOAGGREGATION;
            if (!ppv) return E_POINTER;
            *ppv = nullptr;
            PcamFilter* filter = nullptr;
            try { filter = new PcamFilter(); }
            catch (...) { return E_OUTOFMEMORY; }
            if (!filter) return E_OUTOFMEMORY;
            const HRESULT hr = filter->QueryInterface(riid, ppv);
            filter->Release();
            return hr;
        }
        STDMETHODIMP LockServer(BOOL) override { return S_OK; }

    private:
        std::atomic<ULONG> m_ref{ 1 };
    };
}

bool PcamDirectShowCanUnload() noexcept
{
    return g_objectCount.load(std::memory_order_acquire) == 0;
}

HRESULT PcamDirectShowGetClassObject(REFIID riid, void** result)
{    if (!result) return E_POINTER;
    *result = nullptr;
    auto* factory = new (std::nothrow) PcamClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(riid, result);
    factory->Release();
    return hr;
}

HRESULT PcamDirectShowRegister(HMODULE module)
{
    if (!module) return E_INVALIDARG;
    ComInitializer com;

    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module, path, ARRAYSIZE(path));
    if (!length || length == ARRAYSIZE(path)) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);

    wchar_t key[256]{};
    HRESULT hr = StringCchPrintfW(key, ARRAYSIZE(key),
        L"SOFTWARE\\Classes\\CLSID\\%s", PCAM_DSHOW_CLSID_STR);
    if (FAILED(hr)) return hr;

    HKEY root = nullptr;
    // No WOW64 flag: each registration process writes its own registry view.
    // The 64-bit DLL registers the 64-bit view and the 32-bit DLL, loaded by
    // SysWOW64\regsvr32, registers the 32-bit view for 32-bit consumers.
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &root, nullptr);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    status = RegSetValueExW(root, L"FriendlyName", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(L"PCam"), static_cast<DWORD>(sizeof(L"PCam")));
    RegCloseKey(root);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);

    hr = StringCchPrintfW(key, ARRAYSIZE(key),
        L"SOFTWARE\\Classes\\CLSID\\%s\\InprocServer32", PCAM_DSHOW_CLSID_STR);
    if (FAILED(hr)) return hr;

    root = nullptr;
    status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &root, nullptr);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    status = RegSetValueExW(root, nullptr, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(path), static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)));
    if (status == ERROR_SUCCESS)
        status = RegSetValueExW(root, L"ThreadingModel", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(L"Both"), static_cast<DWORD>(sizeof(L"Both")));
    RegCloseKey(root);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);

    Ptr<IFilterMapper2> mapper;
    hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IFilterMapper2), reinterpret_cast<void**>(mapper.put()));
    if (FAILED(hr)) return hr;

    // Register as a capture pin with its category and media types. Enumerators
    // that query the filter mapper (rather than only the device category) need
    // this to recognise the pin.
    REGPINTYPES pinTypes[2]{};
    pinTypes[0].clsMajorType = &MEDIATYPE_Video;
    pinTypes[0].clsMinorType = &MEDIASUBTYPE_YUY2;
    pinTypes[1].clsMajorType = &MEDIATYPE_Video;
    pinTypes[1].clsMinorType = &MEDIASUBTYPE_RGB32;

    REGFILTERPINS2 pinInfo{};
    pinInfo.dwFlags = REG_PINFLAG_B_OUTPUT;
    pinInfo.cInstances = 1;
    pinInfo.nMediaTypes = ARRAYSIZE(pinTypes);
    pinInfo.lpMediaType = pinTypes;
    pinInfo.nMediums = 0;
    pinInfo.lpMedium = nullptr;
    pinInfo.clsPinCategory = &PIN_CATEGORY_CAPTURE;

    REGFILTER2 filterInfo{};
    filterInfo.dwVersion = 2;
    filterInfo.dwMerit = MERIT_DO_NOT_USE;
    filterInfo.cPins2 = 1;
    filterInfo.rgPins2 = &pinInfo;

    return mapper->RegisterFilter(PCAM_DSHOW_CLSID, L"PCam", nullptr,
        &CLSID_VideoInputDeviceCategory, nullptr, &filterInfo);
}

HRESULT PcamDirectShowUnregister()
{
    wchar_t key[256]{};
    HRESULT first = S_OK;

    if (SUCCEEDED(StringCchPrintfW(key, ARRAYSIZE(key),
        L"SOFTWARE\\Classes\\CLSID\\%s\\Instance\\%s", kVideoInputCategory, PCAM_DSHOW_CLSID_STR)))
    {
        const LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, key);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
            first = HRESULT_FROM_WIN32(status);
    }

    if (SUCCEEDED(StringCchPrintfW(key, ARRAYSIZE(key),
        L"SOFTWARE\\Classes\\CLSID\\%s", PCAM_DSHOW_CLSID_STR)))
    {
        const LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, key);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND && SUCCEEDED(first))
            first = HRESULT_FROM_WIN32(status);
    }

    return first;
}

HRESULT PcamDirectShowRegisterSelf() noexcept
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&PcamDirectShowRegisterSelf), &module))
        return HRESULT_FROM_WIN32(GetLastError());
    return PcamDirectShowRegister(module);
}

HRESULT PcamDirectShowUnregisterSelf() noexcept
{
    return PcamDirectShowUnregister();
}
