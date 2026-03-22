// WASAPI audio output implementation
// Renders PCM audio to the default Windows audio output device.

#include "audio_output.h"

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cstdio>
#include <cstring>

// WASAPI GUIDs
static const CLSID CLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C,
    {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID IID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35,
    {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID IID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32,
    {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID IID_IAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483,
    {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

namespace webcam {

struct AudioOutput::Impl {
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    IMMDevice* device = nullptr;
    IMMDeviceEnumerator* enumerator = nullptr;
    uint32_t buffer_frames = 0;
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    uint16_t block_align = 0;
    bool com_init = false;
};

AudioOutput::AudioOutput() : impl_(new Impl), initialized_(false) {}

AudioOutput::~AudioOutput() {
    shutdown();
    delete impl_;
}

bool AudioOutput::init(uint32_t sample_rate, uint16_t channels, uint16_t bits,
                        const std::string& device_name) {
    if (initialized_) return true;

    HRESULT hr;

    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == S_FALSE || hr == RPC_E_CHANGED_MODE) {
        impl_->com_init = true;
    } else {
        fprintf(stderr, "AudioOutput: CoInitialize failed: 0x%08X\n", (unsigned)hr);
        return false;
    }

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                          IID_IMMDeviceEnumerator, (void**)&impl_->enumerator);
    if (FAILED(hr)) {
        fprintf(stderr, "AudioOutput: Failed to create device enumerator: 0x%08X\n", (unsigned)hr);
        return false;
    }

    if (!device_name.empty()) {
        // Find device by name substring
        IMMDeviceCollection* collection = nullptr;
        hr = impl_->enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
        if (SUCCEEDED(hr) && collection) {
            UINT count = 0;
            collection->GetCount(&count);
            for (UINT i = 0; i < count; i++) {
                IMMDevice* dev = nullptr;
                collection->Item(i, &dev);
                if (!dev) continue;
                IPropertyStore* props = nullptr;
                dev->OpenPropertyStore(STGM_READ, &props);
                if (props) {
                    PROPVARIANT name;
                    PropVariantInit(&name);
                    // PKEY_Device_FriendlyName
                    PROPERTYKEY key = {{0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}},14};
                    if (SUCCEEDED(props->GetValue(key, &name)) && name.vt == VT_LPWSTR) {
                        // Convert wide string to narrow for comparison
                        char narrow[256] = {0};
                        WideCharToMultiByte(CP_UTF8, 0, name.pwszVal, -1, narrow, 255, nullptr, nullptr);
                        if (strstr(narrow, device_name.c_str())) {
                            impl_->device = dev;
                            fprintf(stderr, "AudioOutput: selected device: %s\n", narrow);
                            PropVariantClear(&name);
                            props->Release();
                            break;
                        }
                    }
                    PropVariantClear(&name);
                    props->Release();
                }
                dev->Release();
            }
            collection->Release();
        }
        if (!impl_->device) {
            fprintf(stderr, "AudioOutput: device '%s' not found, using default\n", device_name.c_str());
        }
    }

    if (!impl_->device) {
        hr = impl_->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &impl_->device);
        if (FAILED(hr)) {
            fprintf(stderr, "AudioOutput: No default audio output device: 0x%08X\n", (unsigned)hr);
            return false;
        }
    }

    hr = impl_->device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                                  (void**)&impl_->client);
    if (FAILED(hr)) {
        fprintf(stderr, "AudioOutput: Failed to activate audio client: 0x%08X\n", (unsigned)hr);
        return false;
    }

    // Use the device's mix format for shared mode compatibility.
    // We'll convert our mono input to match at write time.
    WAVEFORMATEX* mix_fmt = nullptr;
    hr = impl_->client->GetMixFormat(&mix_fmt);
    if (FAILED(hr) || !mix_fmt) {
        fprintf(stderr, "AudioOutput: Failed to get mix format: 0x%08X\n", (unsigned)hr);
        return false;
    }

    fprintf(stderr, "AudioOutput: device mix format: %u Hz, %u ch, %u bit\n",
            mix_fmt->nSamplesPerSec, mix_fmt->nChannels, mix_fmt->wBitsPerSample);

    REFERENCE_TIME duration = 1000000; // 100ms

    hr = impl_->client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                    duration, 0, mix_fmt, nullptr);
    impl_->channels = mix_fmt->nChannels;
    impl_->block_align = mix_fmt->nBlockAlign;
    impl_->sample_rate = mix_fmt->nSamplesPerSec;
    CoTaskMemFree(mix_fmt);
    if (FAILED(hr)) {
        fprintf(stderr, "AudioOutput: Failed to initialize audio client: 0x%08X\n", (unsigned)hr);
        return false;
    }

    hr = impl_->client->GetBufferSize(&impl_->buffer_frames);
    if (FAILED(hr)) {
        fprintf(stderr, "AudioOutput: Failed to get buffer size: 0x%08X\n", (unsigned)hr);
        return false;
    }

    hr = impl_->client->GetService(IID_IAudioRenderClient, (void**)&impl_->render);
    if (FAILED(hr)) {
        fprintf(stderr, "AudioOutput: Failed to get render client: 0x%08X\n", (unsigned)hr);
        return false;
    }

    impl_->sample_rate = sample_rate;
    impl_->channels = channels;
    // block_align already set above from mix_fmt

    hr = impl_->client->Start();
    if (FAILED(hr)) {
        fprintf(stderr, "AudioOutput: Failed to start audio client: 0x%08X\n", (unsigned)hr);
        return false;
    }

    initialized_ = true;
    fprintf(stderr, "AudioOutput: initialized (%u Hz, %u ch, %u bit, buffer %u frames)\n",
            sample_rate, channels, bits, impl_->buffer_frames);
    return true;
}

void AudioOutput::write(const uint8_t* data, size_t size) {
    if (!initialized_ || !data || size == 0) return;

    // Input: mono 16-bit PCM at 44100Hz (2 bytes per sample)
    const int16_t* src = reinterpret_cast<const int16_t*>(data);
    uint32_t input_samples = static_cast<uint32_t>(size / 2);
    if (input_samples == 0) return;

    // Resample 44100→device rate (nearest neighbor) + mono→stereo
    uint32_t output_frames = (uint32_t)((uint64_t)input_samples * impl_->sample_rate / 44100);
    if (output_frames == 0) return;

    // Check available space
    UINT32 padding = 0;
    HRESULT hr = impl_->client->GetCurrentPadding(&padding);
    if (FAILED(hr)) return;

    uint32_t available = impl_->buffer_frames - padding;
    if (output_frames > available) output_frames = available;
    if (output_frames == 0) return;

    BYTE* buf = nullptr;
    hr = impl_->render->GetBuffer(output_frames, &buf);
    if (FAILED(hr) || !buf) return;

    if (impl_->block_align == 8 && impl_->channels == 2) {
        // Stereo 32-bit float — most common WASAPI shared mode
        float* dst = reinterpret_cast<float*>(buf);
        for (uint32_t i = 0; i < output_frames; i++) {
            uint32_t src_idx = (uint32_t)((uint64_t)i * 44100 / impl_->sample_rate);
            if (src_idx >= input_samples) src_idx = input_samples - 1;
            float sample = src[src_idx] / 32768.0f;
            dst[i * 2] = sample;
            dst[i * 2 + 1] = sample;
        }
    } else if (impl_->block_align == 4 && impl_->channels == 2) {
        // Stereo 16-bit PCM
        int16_t* dst = reinterpret_cast<int16_t*>(buf);
        for (uint32_t i = 0; i < output_frames; i++) {
            uint32_t src_idx = (uint32_t)((uint64_t)i * 44100 / impl_->sample_rate);
            if (src_idx >= input_samples) src_idx = input_samples - 1;
            dst[i * 2] = src[src_idx];
            dst[i * 2 + 1] = src[src_idx];
        }
    } else {
        memset(buf, 0, output_frames * impl_->block_align);
    }

    impl_->render->ReleaseBuffer(output_frames, 0);
}

void AudioOutput::shutdown() {
    if (!initialized_) return;

    if (impl_->client) {
        impl_->client->Stop();
        impl_->client->Release();
        impl_->client = nullptr;
    }
    if (impl_->render) {
        impl_->render->Release();
        impl_->render = nullptr;
    }
    if (impl_->device) {
        impl_->device->Release();
        impl_->device = nullptr;
    }
    if (impl_->enumerator) {
        impl_->enumerator->Release();
        impl_->enumerator = nullptr;
    }
    if (impl_->com_init) {
        CoUninitialize();
        impl_->com_init = false;
    }
    initialized_ = false;
}

} // namespace webcam

#else
// Stub for non-Windows
namespace webcam {
AudioOutput::AudioOutput() : impl_(nullptr), initialized_(false) {}
AudioOutput::~AudioOutput() {}
bool AudioOutput::init(uint32_t, uint16_t, uint16_t) { return false; }
void AudioOutput::write(const uint8_t*, size_t) {}
void AudioOutput::shutdown() {}
}
#endif
