// CHDK Microphone — DirectShow audio source filter
// Reads PCM audio from shared memory ("CHDKMicrophoneAudio")
// and presents it as a capture device named "CHDK Microphone".
//
// Build: cl /LD chdk_mic.cpp /link ole32.lib oleaut32.lib strmiids.lib uuid.lib
// Register: regsvr32 chdk_mic.dll
// Unregister: regsvr32 /u chdk_mic.dll

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <strmif.h>
#include <uuids.h>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <vfwmsgs.h>

// {B5F7E42A-EC4B-4C7F-9B3E-1A2D3C4E5F60}
DEFINE_GUID(CLSID_CHDKMic,
    0xB5F7E42A, 0xEC4B, 0x4C7F,
    0x9B, 0x3E, 0x1A, 0x2D, 0x3C, 0x4E, 0x5F, 0x60);

static HMODULE g_hModule = nullptr;
static volatile LONG g_refCount = 0;

// Shared memory constants (must match virtual_mic.cpp)
static constexpr uint32_t MAGIC = 0x4D494348;
static constexpr int HEADER_SIZE = 32;
static constexpr const char* SHMEM_NAME = "CHDKMicrophoneAudio";

// ============================================================
// Minimal DirectShow audio source filter
// ============================================================

// Forward declarations
class CHDKMicPin;
static AM_MEDIA_TYPE* CreateAudioMediaType();
static void FillWaveFormat(WAVEFORMATEX* wfx);

class CHDKMicFilter : public IBaseFilter {
public:
    CHDKMicFilter();
    ~CHDKMicFilter();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    // IPersist
    STDMETHODIMP GetClassID(CLSID* pClsID) override {
        *pClsID = CLSID_CHDKMic;
        return S_OK;
    }

    // IMediaFilter
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Run(REFERENCE_TIME tStart) override;
    STDMETHODIMP GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) override {
        *State = m_state;
        return S_OK;
    }
    STDMETHODIMP SetSyncSource(IReferenceClock* pClock) override {
        m_clock = pClock;
        return S_OK;
    }
    STDMETHODIMP GetSyncSource(IReferenceClock** ppClock) override {
        *ppClock = m_clock;
        if (m_clock) m_clock->AddRef();
        return S_OK;
    }

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** ppEnum) override;
    STDMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin) override;
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) override;
    STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) override;
    STDMETHODIMP QueryVendorInfo(LPWSTR* pVendorInfo) override { return E_NOTIMPL; }

    CHDKMicPin* m_pin;
    FILTER_STATE m_state;
    IFilterGraph* m_graph;
    IReferenceClock* m_clock;
    LONG m_ref;
    WCHAR m_name[128];
};

class CHDKMicPin : public IPin, public IAMStreamConfig, public IKsPropertySet {
public:
    CHDKMicPin(CHDKMicFilter* filter);
    ~CHDKMicPin();

    // IUnknown — delegate to filter
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return m_filter->AddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return m_filter->Release(); }

    // IPin
    STDMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin** ppPin) override;
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* pPinDir) override {
        *pPinDir = PINDIR_OUTPUT;
        return S_OK;
    }
    STDMETHODIMP QueryId(LPWSTR* Id) override;
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum) override;
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() override { return S_OK; }
    STDMETHODIMP BeginFlush() override { return S_OK; }
    STDMETHODIMP EndFlush() override { return S_OK; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override { return S_OK; }
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) override;
    STDMETHODIMP GetNumberOfCapabilities(int* piCount, int* piSize) override;
    STDMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) override;

    // IKsPropertySet (required for audio capture device enumeration)
    STDMETHODIMP Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP Get(REFGUID guidPropSet, DWORD dwPropID,
                     LPVOID pInstanceData, DWORD cbInstanceData,
                     LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned) override;
    STDMETHODIMP QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) override;

    void ThreadProc();

    CHDKMicFilter* m_filter;
    IPin* m_connected;
    IMemAllocator* m_allocator;
    IMemInputPin* m_input_pin;
    AM_MEDIA_TYPE m_mt;
    HANDLE m_thread;
    volatile bool m_running;

    // Shared memory
    HANDLE m_hMap;
    uint8_t* m_shm;
    uint32_t m_last_read_pos;
};

// ============================================================
// Implementation
// ============================================================

CHDKMicFilter::CHDKMicFilter() : m_ref(1), m_state(State_Stopped),
    m_graph(nullptr), m_clock(nullptr) {
    m_pin = new CHDKMicPin(this);
    wcscpy_s(m_name, L"CHDK Microphone");
    InterlockedIncrement(&g_refCount);
}

CHDKMicFilter::~CHDKMicFilter() {
    delete m_pin;
    InterlockedDecrement(&g_refCount);
}

STDMETHODIMP CHDKMicFilter::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IPersist ||
        riid == IID_IMediaFilter || riid == IID_IBaseFilter) {
        *ppv = static_cast<IBaseFilter*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP CHDKMicFilter::Stop() {
    m_pin->m_running = false;
    if (m_pin->m_thread) {
        WaitForSingleObject(m_pin->m_thread, 3000);
        CloseHandle(m_pin->m_thread);
        m_pin->m_thread = nullptr;
    }
    m_state = State_Stopped;
    return S_OK;
}

STDMETHODIMP CHDKMicFilter::Pause() {
    m_state = State_Paused;
    return S_OK;
}

STDMETHODIMP CHDKMicFilter::Run(REFERENCE_TIME) {
    if (m_pin->m_connected && m_pin->m_input_pin) {
        m_pin->m_running = true;
        m_pin->m_thread = CreateThread(nullptr, 0,
            [](LPVOID p) -> DWORD { ((CHDKMicPin*)p)->ThreadProc(); return 0; },
            m_pin, 0, nullptr);
    }
    m_state = State_Running;
    return S_OK;
}

// Minimal IEnumPins
class CHDKEnumPins : public IEnumPins {
    LONG m_ref = 1;
    int m_pos = 0;
    CHDKMicFilter* m_filter;
public:
    CHDKEnumPins(CHDKMicFilter* f) : m_filter(f) { f->AddRef(); }
    ~CHDKEnumPins() { m_filter->Release(); }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IEnumPins) {
            *ppv = this; AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP Next(ULONG c, IPin** pp, ULONG* pf) override {
        if (m_pos == 0 && c >= 1) {
            *pp = m_filter->m_pin;
            m_filter->m_pin->AddRef();
            m_pos++;
            if (pf) *pf = 1;
            return (c == 1) ? S_OK : S_FALSE;
        }
        if (pf) *pf = 0;
        return S_FALSE;
    }
    STDMETHODIMP Skip(ULONG c) override { m_pos += c; return (m_pos <= 1) ? S_OK : S_FALSE; }
    STDMETHODIMP Reset() override { m_pos = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumPins** pp) override { *pp = new CHDKEnumPins(m_filter); return S_OK; }
};

STDMETHODIMP CHDKMicFilter::EnumPins(IEnumPins** ppEnum) {
    *ppEnum = new CHDKEnumPins(this);
    return S_OK;
}

STDMETHODIMP CHDKMicFilter::FindPin(LPCWSTR Id, IPin** ppPin) {
    *ppPin = m_pin;
    m_pin->AddRef();
    return S_OK;
}

STDMETHODIMP CHDKMicFilter::QueryFilterInfo(FILTER_INFO* pInfo) {
    wcscpy_s(pInfo->achName, m_name);
    pInfo->pGraph = m_graph;
    if (m_graph) m_graph->AddRef();
    return S_OK;
}

STDMETHODIMP CHDKMicFilter::JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) {
    m_graph = pGraph;
    if (pName) wcscpy_s(m_name, pName);
    return S_OK;
}

// ============================================================
// Pin implementation
// ============================================================

CHDKMicPin::CHDKMicPin(CHDKMicFilter* filter)
    : m_filter(filter), m_connected(nullptr), m_allocator(nullptr),
      m_input_pin(nullptr), m_thread(nullptr), m_running(false),
      m_hMap(nullptr), m_shm(nullptr), m_last_read_pos(0) {
    memset(&m_mt, 0, sizeof(m_mt));
}

CHDKMicPin::~CHDKMicPin() {
    Disconnect();
    if (m_shm) { UnmapViewOfFile(m_shm); m_shm = nullptr; }
    if (m_hMap) { CloseHandle(m_hMap); m_hMap = nullptr; }
}

STDMETHODIMP CHDKMicPin::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IPin) {
        *ppv = static_cast<IPin*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_IAMStreamConfig) {
        *ppv = static_cast<IAMStreamConfig*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_IKsPropertySet) {
        *ppv = static_cast<IKsPropertySet*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

static void FillWaveFormat(WAVEFORMATEX* wfx) {
    memset(wfx, 0, sizeof(WAVEFORMATEX));
    wfx->wFormatTag = WAVE_FORMAT_PCM;
    wfx->nChannels = 1;
    wfx->nSamplesPerSec = 44100;
    wfx->wBitsPerSample = 16;
    wfx->nBlockAlign = 2;
    wfx->nAvgBytesPerSec = 88200;
}

static AM_MEDIA_TYPE* CreateAudioMediaType() {
    AM_MEDIA_TYPE* pmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    memset(pmt, 0, sizeof(AM_MEDIA_TYPE));
    pmt->majortype = MEDIATYPE_Audio;
    pmt->subtype = MEDIASUBTYPE_PCM;
    pmt->bFixedSizeSamples = TRUE;
    pmt->lSampleSize = 2; // 16-bit mono
    pmt->formattype = FORMAT_WaveFormatEx;
    pmt->cbFormat = sizeof(WAVEFORMATEX);
    pmt->pbFormat = (BYTE*)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
    FillWaveFormat((WAVEFORMATEX*)pmt->pbFormat);
    return pmt;
}

STDMETHODIMP CHDKMicPin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE*) {
    AM_MEDIA_TYPE* pmt = CreateAudioMediaType();
    HRESULT hr = pReceivePin->ReceiveConnection(this, pmt);
    if (FAILED(hr)) {
        CoTaskMemFree(pmt->pbFormat);
        CoTaskMemFree(pmt);
        return hr;
    }
    m_mt = *pmt;
    m_connected = pReceivePin;
    m_connected->AddRef();

    // Get IMemInputPin
    pReceivePin->QueryInterface(IID_IMemInputPin, (void**)&m_input_pin);

    // Set up allocator
    if (m_input_pin) {
        m_input_pin->GetAllocator(&m_allocator);
        if (m_allocator) {
            ALLOCATOR_PROPERTIES props = {4, 8820, 1, 0}; // 4 buffers, ~100ms each
            ALLOCATOR_PROPERTIES actual;
            m_allocator->SetProperties(&props, &actual);
            m_allocator->Commit();
        }
    }

    CoTaskMemFree(pmt);

    // Open shared memory
    m_hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, SHMEM_NAME);
    if (m_hMap) {
        m_shm = (uint8_t*)MapViewOfFile(m_hMap, FILE_MAP_READ, 0, 0, 0);
    }

    return S_OK;
}

STDMETHODIMP CHDKMicPin::Disconnect() {
    if (m_allocator) { m_allocator->Decommit(); m_allocator->Release(); m_allocator = nullptr; }
    if (m_input_pin) { m_input_pin->Release(); m_input_pin = nullptr; }
    if (m_connected) { m_connected->Release(); m_connected = nullptr; }
    return S_OK;
}

STDMETHODIMP CHDKMicPin::ConnectedTo(IPin** ppPin) {
    if (!m_connected) return VFW_E_NOT_CONNECTED;
    *ppPin = m_connected;
    m_connected->AddRef();
    return S_OK;
}

STDMETHODIMP CHDKMicPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
    if (!m_connected) return VFW_E_NOT_CONNECTED;
    *pmt = m_mt;
    if (m_mt.pbFormat) {
        pmt->pbFormat = (BYTE*)CoTaskMemAlloc(m_mt.cbFormat);
        memcpy(pmt->pbFormat, m_mt.pbFormat, m_mt.cbFormat);
    }
    return S_OK;
}

STDMETHODIMP CHDKMicPin::QueryPinInfo(PIN_INFO* pInfo) {
    pInfo->pFilter = m_filter;
    m_filter->AddRef();
    pInfo->dir = PINDIR_OUTPUT;
    wcscpy_s(pInfo->achName, L"Audio");
    return S_OK;
}

STDMETHODIMP CHDKMicPin::QueryId(LPWSTR* Id) {
    *Id = (LPWSTR)CoTaskMemAlloc(12);
    wcscpy_s(*Id, 6, L"Audio");
    return S_OK;
}

STDMETHODIMP CHDKMicPin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
    if (pmt->majortype == MEDIATYPE_Audio && pmt->subtype == MEDIASUBTYPE_PCM)
        return S_OK;
    return S_FALSE;
}

// Minimal IEnumMediaTypes
class CHDKEnumMT : public IEnumMediaTypes {
    LONG m_ref = 1;
    int m_pos = 0;
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
            *ppv = this; AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP Next(ULONG c, AM_MEDIA_TYPE** pp, ULONG* pf) override {
        if (m_pos == 0 && c >= 1) {
            *pp = CreateAudioMediaType();
            m_pos++;
            if (pf) *pf = 1;
            return (c == 1) ? S_OK : S_FALSE;
        }
        if (pf) *pf = 0;
        return S_FALSE;
    }
    STDMETHODIMP Skip(ULONG c) override { m_pos += c; return S_OK; }
    STDMETHODIMP Reset() override { m_pos = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumMediaTypes** pp) override { *pp = new CHDKEnumMT(); return S_OK; }
};

STDMETHODIMP CHDKMicPin::EnumMediaTypes(IEnumMediaTypes** ppEnum) {
    *ppEnum = new CHDKEnumMT();
    return S_OK;
}

// IAMStreamConfig
STDMETHODIMP CHDKMicPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
    *ppmt = CreateAudioMediaType();
    return S_OK;
}

STDMETHODIMP CHDKMicPin::GetNumberOfCapabilities(int* piCount, int* piSize) {
    *piCount = 1;
    *piSize = sizeof(AUDIO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP CHDKMicPin::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
    if (iIndex != 0) return S_FALSE;
    *ppmt = CreateAudioMediaType();
    AUDIO_STREAM_CONFIG_CAPS* caps = (AUDIO_STREAM_CONFIG_CAPS*)pSCC;
    memset(caps, 0, sizeof(*caps));
    caps->guid = MEDIATYPE_Audio;
    caps->MinimumChannels = 1;
    caps->MaximumChannels = 1;
    caps->ChannelsGranularity = 1;
    caps->MinimumBitsPerSample = 16;
    caps->MaximumBitsPerSample = 16;
    caps->BitsPerSampleGranularity = 16;
    caps->MinimumSampleFrequency = 44100;
    caps->MaximumSampleFrequency = 44100;
    caps->SampleFrequencyGranularity = 0;
    return S_OK;
}

// IKsPropertySet — needed for audio capture device category enumeration
STDMETHODIMP CHDKMicPin::Get(REFGUID guidPropSet, DWORD dwPropID,
                              LPVOID, DWORD, LPVOID pPropData,
                              DWORD cbPropData, DWORD* pcbReturned) {
    if (IsEqualGUID(guidPropSet, AMPROPSETID_Pin) && dwPropID == 0) {
        if (cbPropData >= sizeof(GUID)) {
            *(GUID*)pPropData = PIN_CATEGORY_CAPTURE;
            if (pcbReturned) *pcbReturned = sizeof(GUID);
            return S_OK;
        }
    }
    return E_NOTIMPL;
}

STDMETHODIMP CHDKMicPin::QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) {
    if (IsEqualGUID(guidPropSet, AMPROPSETID_Pin) && dwPropID == 0) {
        *pTypeSupport = KSPROPERTY_SUPPORT_GET;
        return S_OK;
    }
    return E_NOTIMPL;
}

// Worker thread — reads from shared memory, delivers to downstream filter
void CHDKMicPin::ThreadProc() {
    while (m_running) {
        if (!m_shm || !m_input_pin || !m_allocator) {
            Sleep(10);
            continue;
        }

        // Check magic
        if (*(volatile uint32_t*)m_shm != MAGIC) {
            Sleep(10);
            continue;
        }

        {
            uint32_t ring_size = *(volatile uint32_t*)(m_shm + 20);
            uint32_t wp = *(volatile uint32_t*)(m_shm + 12);
            uint8_t* ring = m_shm + HEADER_SIZE;
            uint32_t avail = (wp >= m_last_read_pos) ?
                wp - m_last_read_pos : ring_size - m_last_read_pos + wp;

            if (avail < 2940) {
                Sleep(10);
                continue;
            }

            IMediaSample* sample = nullptr;
            HRESULT hr = m_allocator->GetBuffer(&sample, nullptr, nullptr, 0);
            if (FAILED(hr) || !sample) { Sleep(1); continue; }

            BYTE* buf = nullptr;
            sample->GetPointer(&buf);
            long max_len = sample->GetSize();
            long copy_len = ((long)avail < max_len) ? (long)avail : max_len;

            uint32_t pos = m_last_read_pos;
            for (long i = 0; i < copy_len; i++) {
                buf[i] = ring[pos];
                pos = (pos + 1) % ring_size;
            }
            m_last_read_pos = pos;

            sample->SetActualDataLength(copy_len);
            m_input_pin->Receive(sample);
            sample->Release();
        }
    }
}

// ============================================================
// COM Class Factory
// ============================================================

class CHDKMicClassFactory : public IClassFactory {
    LONG m_ref = 1;
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = this; AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP CreateInstance(IUnknown*, REFIID riid, void** ppv) override {
        CHDKMicFilter* f = new CHDKMicFilter();
        HRESULT hr = f->QueryInterface(riid, ppv);
        f->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) InterlockedIncrement(&g_refCount);
        else InterlockedDecrement(&g_refCount);
        return S_OK;
    }
};

// ============================================================
// DLL exports
// ============================================================

extern "C" {

BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hModule = hInstDLL;
        DisableThreadLibraryCalls(hInstDLL);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (rclsid == CLSID_CHDKMic) {
        CHDKMicClassFactory* cf = new CHDKMicClassFactory();
        HRESULT hr = cf->QueryInterface(riid, ppv);
        cf->Release();
        return hr;
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() {
    return (g_refCount == 0) ? S_OK : S_FALSE;
}

// Registry helpers
static HRESULT SetRegKey(HKEY root, const char* path, const char* name, const char* value) {
    HKEY hKey;
    if (RegCreateKeyExA(root, path, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
        return E_FAIL;
    RegSetValueExA(hKey, name, 0, REG_SZ, (const BYTE*)value, (DWORD)strlen(value) + 1);
    RegCloseKey(hKey);
    return S_OK;
}

STDAPI DllRegisterServer() {
    char path[MAX_PATH];
    GetModuleFileNameA(g_hModule, path, MAX_PATH);

    // Register COM class
    SetRegKey(HKEY_CLASSES_ROOT,
        "CLSID\\{B5F7E42A-EC4B-4C7F-9B3E-1A2D3C4E5F60}",
        nullptr, "CHDK Microphone");
    SetRegKey(HKEY_CLASSES_ROOT,
        "CLSID\\{B5F7E42A-EC4B-4C7F-9B3E-1A2D3C4E5F60}\\InprocServer32",
        nullptr, path);
    SetRegKey(HKEY_CLASSES_ROOT,
        "CLSID\\{B5F7E42A-EC4B-4C7F-9B3E-1A2D3C4E5F60}\\InprocServer32",
        "ThreadingModel", "Both");

    // Register as audio capture device
    // CLSID_AudioInputDeviceCategory = {33D9A762-90C8-11D0-BD43-00A0C911CE86}
    char filterPath[512];
    snprintf(filterPath, sizeof(filterPath),
        "CLSID\\{33D9A762-90C8-11D0-BD43-00A0C911CE86}\\Instance\\{B5F7E42A-EC4B-4C7F-9B3E-1A2D3C4E5F60}");
    SetRegKey(HKEY_CLASSES_ROOT, filterPath, "CLSID", "{B5F7E42A-EC4B-4C7F-9B3E-1A2D3C4E5F60}");
    SetRegKey(HKEY_CLASSES_ROOT, filterPath, "FriendlyName", "CHDK Microphone");

    return S_OK;
}

STDAPI DllUnregisterServer() {
    RegDeleteTreeA(HKEY_CLASSES_ROOT,
        "CLSID\\{B5F7E42A-EC4B-4C7F-9B3E-1A2D3C4E5F60}");
    RegDeleteTreeA(HKEY_CLASSES_ROOT,
        "CLSID\\{33D9A762-90C8-11D0-BD43-00A0C911CE86}\\Instance\\{B5F7E42A-EC4B-4C7F-9B3E-1A2D3C4E5F60}");
    return S_OK;
}

} // extern "C"
