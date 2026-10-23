#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#ifdef WINVER
#undef WINVER
#endif
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00

#include <SKSE/SKSE.h>
#include "media.h"
#include <d3d11_4.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <emmintrin.h>
#include <wincodec.h>
#include <xaudio2.h>

#include <webp/decode.h>
#include <webp/demux.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <chrono>
#include <initializer_list>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

using Microsoft::WRL::ComPtr;

namespace
{
	std::wstring Lower(std::wstring value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
			return static_cast<wchar_t>(towlower(c));
		});
		return value;
	}

	std::wstring ExtensionOf(const std::filesystem::path& path)
	{
		return Lower(path.extension().wstring());
	}

	bool IsOneOf(const std::wstring& ext, std::initializer_list<const wchar_t*> names)
	{
		for (const wchar_t* name : names) {
			if (ext == name) {
				return true;
			}
		}
		return false;
	}

	bool IsStillImage(const std::wstring& ext)
	{
		return IsOneOf(ext, { L".png", L".jpg", L".jpeg", L".bmp", L".tga", L".gif", L".psd" });
	}

	bool IsVideo(const std::wstring& ext)
	{
		return IsOneOf(ext, { L".mp4", L".m4v", L".mov", L".avi", L".wmv", L".asf", L".mkv", L".webm", L".flv", L".mpg", L".mpeg", L".m2ts", L".ts", L".3gp" });
	}

	bool IsAudio(const std::wstring& ext)
	{
		return IsOneOf(ext, { L".mp3", L".wav", L".m4a", L".wma", L".flac", L".aac", L".ogg", L".opus" });
	}

	std::string Narrow(const std::filesystem::path& path)
	{
		const auto name = path.filename().wstring();
		if (name.empty()) {
			return {};
		}
		const int size = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<std::size_t>(size), '\0');
		if (size > 0) {
			WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), out.data(), size, nullptr, nullptr);
		}
		return out;
	}

	std::vector<unsigned char> ReadFile(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
	}

	class VoiceEvents final : public IXAudio2VoiceCallback
	{
	public:
		struct Block
		{
			std::vector<BYTE> bytes;
			std::atomic<bool> released{ false };
		};

		static void Release(void* context)
		{
			auto* block = static_cast<Block*>(context);
			if (block && !block->released.exchange(true)) {
				delete block;
			}
		}

		void OnVoiceProcessingPassStart(UINT32) override {}
		void OnVoiceProcessingPassEnd() override {}
		void OnStreamEnd() override {}
		void OnBufferStart(void*) override {}
		void OnBufferEnd(void* context) override { Release(context); }
		void OnLoopEnd(void*) override {}
		void OnVoiceError(void* context, HRESULT) override { Release(context); }
	};

	void StartupMediaFoundation()
	{
		static std::once_flag once;
		std::call_once(once, []() {
			if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
				SKSE::log::error("Media Foundation не запустился");
			}
		});
	}

	int RotationTurns(IMFMediaType* type)
	{
		if (!type) {
			return 0;
		}
		UINT32 rotation = 0;
		if (FAILED(type->GetUINT32(MF_MT_VIDEO_ROTATION, &rotation))) {
			return 0;
		}
		if (rotation == 90) {
			return 1;
		}
		if (rotation == 180) {
			return 2;
		}
		if (rotation == 270) {
			return 3;
		}
		return 0;
	}

	bool VisibleSize(IMFMediaType* type, UINT32& width, UINT32& height)
	{
		if (!type) {
			return false;
		}
		UINT32 bytes = 0;
		MFVideoArea* area = nullptr;
		if (FAILED(type->GetAllocatedBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8**>(&area), &bytes)) || !area || bytes < sizeof(MFVideoArea)) {
			return false;
		}
		const bool ok = area->Area.cx > 0 && area->Area.cy > 0;
		if (ok) {
			width = static_cast<UINT32>(area->Area.cx);
			height = static_cast<UINT32>(area->Area.cy);
		}
		CoTaskMemFree(area);
		return ok;
	}

	long long Gcd(long long a, long long b)
	{
		while (b != 0) {
			const long long t = a % b;
			a = b;
			b = t;
		}
		return a < 0 ? -a : a;
	}

	unsigned MetadataUInt(IWICMetadataQueryReader* meta, const wchar_t* name, unsigned fallback)
	{
		if (!meta) {
			return fallback;
		}
		PROPVARIANT value;
		PropVariantInit(&value);
		unsigned result = fallback;
		if (SUCCEEDED(meta->GetMetadataByName(name, &value))) {
			if (value.vt == VT_UI1) {
				result = value.bVal;
			} else if (value.vt == VT_UI2) {
				result = value.uiVal;
			} else if (value.vt == VT_UI4) {
				result = value.ulVal;
			}
		}
		PropVariantClear(&value);
		return result;
	}

	void FillOpaque(std::vector<unsigned char>& canvas, const unsigned char color[4])
	{
		for (std::size_t i = 0; i + 3 < canvas.size(); i += 4) {
			std::memcpy(canvas.data() + i, color, 4);
		}
	}

	void ClearRect(std::vector<unsigned char>& canvas, unsigned canvasW, unsigned canvasH, unsigned x, unsigned y, unsigned w, unsigned h, const unsigned char color[4])
	{
		if (w == 0 || h == 0 || x >= canvasW || y >= canvasH) {
			return;
		}
		const unsigned x1 = static_cast<unsigned>(std::min(static_cast<std::size_t>(canvasW), static_cast<std::size_t>(x) + w));
		const unsigned y1 = static_cast<unsigned>(std::min(static_cast<std::size_t>(canvasH), static_cast<std::size_t>(y) + h));
		for (unsigned row = y; row < y1; ++row) {
			unsigned char* dest = canvas.data() + (static_cast<std::size_t>(row) * canvasW + x) * 4;
			for (unsigned col = x; col < x1; ++col) {
				std::memcpy(dest, color, 4);
				dest += 4;
			}
		}
	}

	void BlitOpaque(std::vector<unsigned char>& canvas, unsigned canvasW, unsigned canvasH, unsigned dx, unsigned dy, const unsigned char* src, unsigned sw, unsigned sh)
	{
		if (!src || sw == 0 || sh == 0) {
			return;
		}
		for (unsigned row = 0; row < sh; ++row) {
			const std::size_t y = static_cast<std::size_t>(dy) + row;
			if (y >= canvasH) {
				break;
			}
			const unsigned char* srcRow = src + static_cast<std::size_t>(row) * sw * 4;
			unsigned char* destRow = canvas.data() + (y * canvasW) * 4;
			for (unsigned col = 0; col < sw; ++col) {
				const std::size_t x = static_cast<std::size_t>(dx) + col;
				const unsigned char* pixel = srcRow + static_cast<std::size_t>(col) * 4;
				if (x >= canvasW || pixel[3] == 0) {
					continue;
				}
				unsigned char* dest = destRow + x * 4;
				dest[0] = pixel[0];
				dest[1] = pixel[1];
				dest[2] = pixel[2];
				dest[3] = 255;
			}
		}
	}

	void GifBackground(IWICImagingFactory* factory, IWICBitmapDecoder* decoder, unsigned char color[4])
	{
		color[0] = 0;
		color[1] = 0;
		color[2] = 0;
		color[3] = 255;
		if (!factory || !decoder) {
			return;
		}
		ComPtr<IWICBitmapFrameDecode> frame;
		ComPtr<IWICPalette> palette;
		if (FAILED(decoder->GetFrame(0, frame.GetAddressOf())) ||
			FAILED(factory->CreatePalette(palette.GetAddressOf())) ||
			FAILED(frame->CopyPalette(palette.Get()))) {
			return;
		}
		WICColor colors[256]{};
		UINT count = 0;
		if (FAILED(palette->GetColors(256, colors, &count)) || count == 0) {
			return;
		}
		unsigned index = 0;
		unsigned transparent = 1000;
		ComPtr<IWICMetadataQueryReader> root;
		ComPtr<IWICMetadataQueryReader> meta;
		if (SUCCEEDED(decoder->GetMetadataQueryReader(root.GetAddressOf()))) {
			index = MetadataUInt(root.Get(), L"/logscrdesc/BackgroundColorIndex", 0);
		}
		if (SUCCEEDED(frame->GetMetadataQueryReader(meta.GetAddressOf()))) {
			transparent = MetadataUInt(meta.Get(), L"/grctlext/TransparentColorIndex", 1000);
		}
		if (index >= count || index == transparent) {
			return;
		}
		const WICColor packed = colors[index];
		color[0] = static_cast<unsigned char>((packed >> 16) & 0xFF);
		color[1] = static_cast<unsigned char>((packed >> 8) & 0xFF);
		color[2] = static_cast<unsigned char>(packed & 0xFF);
	}
}

struct Playback::GpuFrame
{
	ComPtr<IMFSample> sample;
	ComPtr<ID3D11Texture2D> texture;
	UINT slice = 0;
	int width = 0;
	int height = 0;
	std::uint64_t serial = 0;
};

struct Playback::SharedFrame
{
	std::vector<unsigned char> rgba;
	int width = 0;
	int height = 0;
	std::uint64_t serial = 0;
};

struct Playback::State
{
	std::mutex mutex;
	std::condition_variable ready;
	std::shared_ptr<SharedFrame> frame;
	std::atomic<bool> run{ false };
	bool opened = false;
	bool failed = false;
	bool loop = true;
	std::atomic<bool> finished{ false };
	std::atomic<bool> audible{ false };
	float volume = 0.2f;
	bool playClipAudio = false;
	std::filesystem::path externalAudio;
	std::thread worker;
	std::mutex readerMutex;
	Microsoft::WRL::ComPtr<IMFSourceReader> reader;

	VoiceEvents events;
	ComPtr<IXAudio2> audio;
	IXAudio2MasteringVoice* master = nullptr;
	IXAudio2SourceVoice* voice = nullptr;
	std::thread audioWorker;
	std::mutex audioReaderMutex;
	Microsoft::WRL::ComPtr<IMFSourceReader> audioReader;
	std::atomic<std::uint32_t> loopEpoch{ 0 };
	std::atomic<std::uint32_t> audioEpoch{ 0 };
	std::atomic<bool> voiceStarted{ false };
	std::atomic<bool> audioReady{ false };

	std::vector<unsigned char> webpBytes;
	WebPAnimDecoder* webp = nullptr;
	int webpWidth = 0;
	int webpHeight = 0;

	std::shared_ptr<GpuFrame> gpuFrame;
	std::atomic<bool> dynamicUpload{ false };
	std::atomic<int> targetW{ 0 };
	std::atomic<int> targetH{ 0 };
	std::mutex poolMutex;
	std::vector<std::vector<unsigned char>> spare;
	std::string label;
};

struct Playback::GpuDevice
{
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<IMFDXGIDeviceManager> manager;
	UINT token = 0;
	ID3D11Device* paired = nullptr;
	ComPtr<ID3D11VideoDevice> api;
	ComPtr<ID3D11VideoContext> videoContext;
	ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
	ComPtr<ID3D11VideoProcessor> processor;
	ComPtr<ID3D11VideoProcessorOutputView> outputView;
	ComPtr<ID3D11Texture2D> output;
	ComPtr<ID3D11Texture2D> staging;
	bool shared = false;
	int inW = 0;
	int inH = 0;
	int outW = 0;
	int outH = 0;
	bool hd = false;
	bool logged = false;
	bool blitLogged = false;
};

Playback::Playback() = default;

Playback::~Playback()
{
	Close();
}

bool Playback::IsMediaFile(const std::filesystem::path& path)
{
	const auto ext = ExtensionOf(path);
	return IsStillImage(ext) || ext == L".webp" || IsVideo(ext);
}

bool Playback::IsAudioFile(const std::filesystem::path& path)
{
	return IsAudio(ExtensionOf(path));
}

bool Playback::IsEmergencyName(const std::filesystem::path& path)
{
	return Lower(path.stem().wstring()) == L"emergency";
}

bool Playback::Open(ID3D11Device* device, const std::filesystem::path& path, float volume, bool playClipAudio, bool loop, const std::filesystem::path& externalAudio)
{
	Close();
	if (!device) {
		return false;
	}
	state = std::make_unique<State>();
	state->volume = std::clamp(volume, 0.0f, 1.0f);
	state->playClipAudio = playClipAudio;
	state->externalAudio = externalAudio;
	state->loop = loop;

	const auto ext = ExtensionOf(path);
	if (ext == L".gif") {
		if (StartGif(path)) {
			BeginAudio(path);
			return true;
		}
		state = std::make_unique<State>();
		state->volume = std::clamp(volume, 0.0f, 1.0f);
		state->playClipAudio = playClipAudio;
		state->externalAudio = externalAudio;
		state->loop = loop;
		if (!LoadStill(device, path)) {
			return false;
		}
		BeginAudio(path);
		return true;
	}
	if (ext == L".webp") {
		if (!StartWebp(device, path)) {
			return false;
		}
		BeginAudio(path);
		return true;
	}
	if (IsStillImage(ext)) {
		if (!LoadStill(device, path)) {
			return false;
		}
		BeginAudio(path);
		return true;
	}
	if (IsVideo(ext)) {
		return StartVideo(device, path);
	}
	state.reset();
	return false;
}

void Playback::BeginAudio(const std::filesystem::path& mediaPath)
{
	if (!state) {
		return;
	}
	std::filesystem::path source = state->externalAudio;
	if (source.empty() && state->playClipAudio && IsVideo(ExtensionOf(mediaPath))) {
		source = mediaPath;
	}
	if (source.empty()) {
		return;
	}
	StartupMediaFoundation();
	state->run = true;
	state->audioWorker = std::thread([this, source]() {
		try {
			AudioMain(source);
		} catch (...) {
			SKSE::log::error("Звук остановился: {}", Narrow(source));
		}
	});
}

void Playback::Close()
{
	StopWorker();
	ReleaseAudio();
	if (state && state->webp) {
		WebPAnimDecoderDelete(state->webp);
		state->webp = nullptr;
	}
	state.reset();
	ReleaseGpuOutput();
	texture.Reset();
	view.Reset();
	width = 0;
	height = 0;
	fitWidth = 0;
	fitHeight = 0;
	orientation = 0;
	textureDynamic = false;
	shown = 0;
}

void Playback::SetFrameTarget(int frameWidth, int frameHeight)
{
	if (frameWidth < 2 || frameHeight < 2) {
		return;
	}
	pendingW = frameWidth;
	pendingH = frameHeight;
	if (state) {
		state->targetW.store(frameWidth);
		state->targetH.store(frameHeight);
	}
}

void Playback::SetVolume(float volume)
{
	if (!state) {
		return;
	}
	state->volume = std::clamp(volume, 0.0f, 1.0f);
	ApplyVolume();
}

bool Playback::TakeFinished()
{
	if (!state) {
		return false;
	}
	return state->finished.exchange(false);
}

bool Playback::HasAudio() const
{
	return state && state->audible.load();
}

bool Playback::Update(ID3D11Device* device, ID3D11DeviceContext* context, int screenWidth, int screenHeight)
{
	SetFrameTarget(screenWidth, screenHeight);
	if (!state || !device || !context) {
		return view != nullptr;
	}
	std::shared_ptr<GpuFrame> gpuFrame;
	{
		std::lock_guard lock(state->mutex);
		gpuFrame = state->gpuFrame;
	}
	if (gpuFrame) {
		if (gpuFrame->serial != shown) {
			BlitGpu(device, context, screenWidth, screenHeight, gpuFrame);
			shown = gpuFrame->serial;
		}
		return view != nullptr;
	}
	std::shared_ptr<SharedFrame> frame;
	{
		std::lock_guard lock(state->mutex);
		frame = state->frame;
	}
	if (!frame || frame->serial == shown || frame->rgba.empty()) {
		return view != nullptr;
	}
	const bool dynamic = state->dynamicUpload.load();
	if (!texture || textureDynamic != dynamic || width != frame->width || height != frame->height) {
		if (!CreateTexture(device, nullptr, frame->width, frame->height, false)) {
			return false;
		}
	}
	if (textureDynamic) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return false;
		}
		const UINT pitch = static_cast<UINT>(frame->width) * 4;
		auto* destination = static_cast<unsigned char*>(mapped.pData);
		if (mapped.RowPitch == pitch) {
			std::memcpy(destination, frame->rgba.data(), static_cast<std::size_t>(pitch) * static_cast<std::size_t>(frame->height));
		} else {
			for (int y = 0; y < frame->height; ++y) {
				std::memcpy(destination + static_cast<std::size_t>(y) * mapped.RowPitch, frame->rgba.data() + static_cast<std::size_t>(y) * pitch, pitch);
			}
		}
		context->Unmap(texture.Get(), 0);
	} else {
		context->UpdateSubresource(texture.Get(), 0, nullptr, frame->rgba.data(), static_cast<UINT>(frame->width) * 4, 0);
	}
	width = frame->width;
	height = frame->height;
	shown = frame->serial;
	if (dynamic) {
		std::vector<unsigned char> spent;
		{
			std::lock_guard lock(state->mutex);
			if (state->frame == frame) {
				spent.swap(frame->rgba);
			}
		}
		Recycle(std::move(spent));
	}
	return view != nullptr;
}

ID3D11ShaderResourceView* Playback::Texture() const
{
	return view.Get();
}

int Playback::Width() const
{
	return fitWidth > 0 ? fitWidth : width;
}

int Playback::Height() const
{
	return fitHeight > 0 ? fitHeight : height;
}

int Playback::Orientation() const
{
	return orientation;
}

bool Playback::LoadStill(ID3D11Device* device, const std::filesystem::path& path)
{
	const auto bytes = ReadFile(path);
	int w = 0;
	int h = 0;
	int channels = 0;
	unsigned char* pixels = bytes.empty() ? nullptr : stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
	if (!pixels || w <= 0 || h <= 0) {
		if (pixels) {
			stbi_image_free(pixels);
		}
		SKSE::log::error("Картинка не открылась: {}", Narrow(path));
		state.reset();
		return false;
	}
	const bool created = CreateTexture(device, pixels, w, h, true);
	stbi_image_free(pixels);
	if (!created) {
		state.reset();
		return false;
	}
	width = w;
	height = h;
	return true;
}

bool Playback::StartWebp(ID3D11Device* device, const std::filesystem::path& path)
{
	state->webpBytes = ReadFile(path);
	if (state->webpBytes.empty()) {
		SKSE::log::error("WebP не открылся: {}", Narrow(path));
		state.reset();
		return false;
	}
	WebPData data{};
	data.bytes = state->webpBytes.data();
	data.size = state->webpBytes.size();
	WebPAnimDecoderOptions options{};
	WebPAnimDecoderOptionsInit(&options);
	options.color_mode = MODE_RGBA;
	state->webp = WebPAnimDecoderNew(&data, &options);
	WebPAnimInfo info{};
	if (!state->webp || !WebPAnimDecoderGetInfo(state->webp, &info) || info.canvas_width == 0 || info.canvas_height == 0) {
		int w = 0;
		int h = 0;
		unsigned char* pixels = WebPDecodeRGBA(state->webpBytes.data(), state->webpBytes.size(), &w, &h);
		if (state->webp) {
			WebPAnimDecoderDelete(state->webp);
			state->webp = nullptr;
		}
		if (!pixels) {
			SKSE::log::error("WebP не декодировался: {}", Narrow(path));
			state.reset();
			return false;
		}
		const bool created = CreateTexture(device, pixels, w, h, true);
		WebPFree(pixels);
		if (!created) {
			state.reset();
			return false;
		}
		width = w;
		height = h;
		return true;
	}
	if (info.frame_count <= 1) {
		uint8_t* buf = nullptr;
		int timestamp = 0;
		if (!WebPAnimDecoderGetNext(state->webp, &buf, &timestamp)) {
			SKSE::log::error("WebP не декодировался: {}", Narrow(path));
			state.reset();
			return false;
		}
		const bool created = CreateTexture(device, buf, static_cast<int>(info.canvas_width), static_cast<int>(info.canvas_height), true);
		WebPAnimDecoderDelete(state->webp);
		state->webp = nullptr;
		if (!created) {
			state.reset();
			return false;
		}
		width = static_cast<int>(info.canvas_width);
		height = static_cast<int>(info.canvas_height);
		return true;
	}

	state->webpWidth = static_cast<int>(info.canvas_width);
	state->webpHeight = static_cast<int>(info.canvas_height);
	state->run = true;
	state->worker = std::thread([this]() {
		try {
			WebpMain();
		} catch (...) {
			std::lock_guard lock(state->mutex);
			state->failed = true;
			state->run = false;
			state->ready.notify_all();
		}
	});
	std::unique_lock lock(state->mutex);
	state->ready.wait_for(lock, std::chrono::seconds(5), [this]() { return state->opened || state->failed; });
	if (!state->opened) {
		lock.unlock();
		SKSE::log::error("Анимированный WebP не запустился");
		Close();
		return false;
	}
	return true;
}

bool Playback::StartGif(const std::filesystem::path& path)
{
	state->run = true;
	state->worker = std::thread([this, path]() {
		try {
			GifMain(path);
		} catch (...) {
			std::lock_guard lock(state->mutex);
			state->failed = true;
			state->run = false;
			state->ready.notify_all();
		}
	});
	std::unique_lock lock(state->mutex);
	state->ready.wait_for(lock, std::chrono::seconds(5), [this]() { return state->opened || state->failed; });
	if (!state->opened) {
		lock.unlock();
		Close();
		return false;
	}
	return true;
}

void Playback::GifMain(std::filesystem::path path)
{
	HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool ownCom = SUCCEEDED(com);
	auto fail = [this]() {
		std::lock_guard lock(state->mutex);
		state->failed = true;
		state->ready.notify_all();
	};

	ComPtr<IWICImagingFactory> factory;
	ComPtr<IWICBitmapDecoder> decoder;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()))) ||
		FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()))) {
		fail();
		if (ownCom) {
			CoUninitialize();
		}
		return;
	}

	UINT count = 0;
	decoder->GetFrameCount(&count);
	if (count == 0) {
		fail();
		if (ownCom) {
			CoUninitialize();
		}
		return;
	}

	unsigned canvasW = 0;
	unsigned canvasH = 0;
	{
		ComPtr<IWICMetadataQueryReader> root;
		if (SUCCEEDED(decoder->GetMetadataQueryReader(root.GetAddressOf()))) {
			canvasW = MetadataUInt(root.Get(), L"/logscrdesc/Width", 0);
			canvasH = MetadataUInt(root.Get(), L"/logscrdesc/Height", 0);
		}
	}
	if (canvasW == 0 || canvasH == 0) {
		ComPtr<IWICBitmapFrameDecode> frame;
		if (SUCCEEDED(decoder->GetFrame(0, frame.GetAddressOf()))) {
			frame->GetSize(&canvasW, &canvasH);
		}
	}
	if (canvasW == 0 || canvasH == 0 || canvasW > 8192 || canvasH > 8192) {
		fail();
		if (ownCom) {
			CoUninitialize();
		}
		return;
	}

	unsigned char background[4];
	GifBackground(factory.Get(), decoder.Get(), background);
	std::vector<unsigned char> canvas(static_cast<std::size_t>(canvasW) * canvasH * 4);
	FillOpaque(canvas, background);
	std::vector<unsigned char> snapshot;
	unsigned prevLeft = 0;
	unsigned prevTop = 0;
	unsigned prevW = 0;
	unsigned prevH = 0;
	unsigned prevDisposal = 0;

	bool first = true;
	while (state->run) {
		for (UINT index = 0; index < count && state->run; ++index) {
			ComPtr<IWICBitmapFrameDecode> frame;
			ComPtr<IWICFormatConverter> converter;
			if (FAILED(decoder->GetFrame(index, frame.GetAddressOf())) ||
				FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
				FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
				continue;
			}
			UINT frameW = 0;
			UINT frameH = 0;
			converter->GetSize(&frameW, &frameH);
			if (frameW == 0 || frameH == 0 || frameW > 8192 || frameH > 8192) {
				continue;
			}
			std::vector<unsigned char> rgba(static_cast<std::size_t>(frameW) * frameH * 4);
			if (FAILED(converter->CopyPixels(nullptr, frameW * 4, static_cast<UINT>(rgba.size()), rgba.data()))) {
				continue;
			}
			unsigned left = 0;
			unsigned top = 0;
			unsigned disposal = 1;
			int delay = 100;
			ComPtr<IWICMetadataQueryReader> meta;
			if (SUCCEEDED(frame->GetMetadataQueryReader(meta.GetAddressOf()))) {
				left = MetadataUInt(meta.Get(), L"/imgdesc/Left", 0);
				top = MetadataUInt(meta.Get(), L"/imgdesc/Top", 0);
				disposal = MetadataUInt(meta.Get(), L"/grctlext/Disposal", 1);
				const unsigned raw = MetadataUInt(meta.Get(), L"/grctlext/Delay", 0);
				if (raw > 1) {
					delay = static_cast<int>(raw) * 10;
				}
			}
			if (prevDisposal == 2) {
				ClearRect(canvas, canvasW, canvasH, prevLeft, prevTop, prevW, prevH, background);
			} else if (prevDisposal == 3 && !snapshot.empty()) {
				canvas = snapshot;
			}
			if (disposal == 3) {
				snapshot = canvas;
			}
			BlitOpaque(canvas, canvasW, canvasH, left, top, rgba.data(), frameW, frameH);
			prevLeft = left;
			prevTop = top;
			prevW = frameW;
			prevH = frameH;
			prevDisposal = disposal;
			Publish(std::vector<unsigned char>(canvas), static_cast<int>(canvasW), static_cast<int>(canvasH), first);
			first = false;
			std::this_thread::sleep_for(std::chrono::milliseconds(delay));
		}
		if (first) {
			fail();
			break;
		}
		if (!state->loop || count <= 1) {
			if (count > 1 && state->run) {
				state->finished = true;
			}
			break;
		}
	}
	if (ownCom) {
		CoUninitialize();
	}
}

bool Playback::StartVideo(ID3D11Device* device, const std::filesystem::path& path)
{
	StartupMediaFoundation();
	if (!EnsureVideoDevice(device)) {
		SKSE::log::warn("Видеоустройство не создалось, декод на процессоре");
	}
	state->label = Narrow(path);
	state->targetW.store(pendingW);
	state->targetH.store(pendingH);
	state->run = true;
	BeginAudio(path);
	state->worker = std::thread([this, path]() {
		try {
			VideoMain(path);
		} catch (...) {
			std::lock_guard lock(state->mutex);
			state->failed = true;
			state->run = false;
			state->ready.notify_all();
		}
	});
	std::unique_lock lock(state->mutex);
	state->ready.wait_for(lock, std::chrono::seconds(8), [this]() { return state->opened || state->failed; });
	if (!state->opened) {
		lock.unlock();
		Close();
		return false;
	}
	return true;
}

void Playback::Publish(std::vector<unsigned char> rgba, int frameWidth, int frameHeight, bool first)
{
	auto frame = std::make_shared<SharedFrame>();
	frame->rgba = std::move(rgba);
	frame->width = frameWidth;
	frame->height = frameHeight;
	std::lock_guard lock(state->mutex);
	frame->serial = state->frame ? state->frame->serial + 1 : 1;
	state->frame = std::move(frame);
	if (first) {
		state->opened = true;
		state->ready.notify_all();
	}
}

void Playback::WebpMain()
{
	bool first = true;
	int previous = 0;
	while (state->run) {
		uint8_t* buf = nullptr;
		int timestamp = 0;
		if (!WebPAnimDecoderHasMoreFrames(state->webp) || !WebPAnimDecoderGetNext(state->webp, &buf, &timestamp)) {
			if (first) {
				std::lock_guard lock(state->mutex);
				state->failed = true;
				state->ready.notify_all();
				return;
			}
			if (!state->loop) {
				if (state->run) {
					state->finished = true;
				}
				return;
			}
			WebPAnimDecoderReset(state->webp);
			previous = 0;
			continue;
		}
		const int delay = std::max(0, timestamp - previous);
		previous = timestamp;
		const auto start = std::chrono::steady_clock::now();
		std::vector<unsigned char> rgba(static_cast<std::size_t>(state->webpWidth) * state->webpHeight * 4);
		std::memcpy(rgba.data(), buf, rgba.size());
		Publish(std::move(rgba), state->webpWidth, state->webpHeight, first);
		first = false;
		const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
		const int wait = delay - static_cast<int>(spent.count());
		if (wait > 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(wait));
		}
	}
}

void Playback::AudioMain(std::filesystem::path path)
{
	HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	struct CoGuard
	{
		bool own = false;
		~CoGuard()
		{
			if (own) {
				CoUninitialize();
			}
		}
	} guard{ SUCCEEDED(com) };

	ComPtr<IMFAttributes> attributes;
	if (FAILED(MFCreateAttributes(attributes.GetAddressOf(), 2))) {
		return;
	}
	attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
	attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE);

	ComPtr<IMFSourceReader> reader;
	if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), reader.GetAddressOf()))) {
		SKSE::log::warn("Звук видео не открылся: {}", Narrow(path));
		return;
	}
	{
		std::lock_guard lock(state->audioReaderMutex);
		state->audioReader = reader;
	}
	struct ReaderClear
	{
		State* target = nullptr;
		~ReaderClear()
		{
			if (!target) {
				return;
			}
			std::lock_guard lock(target->audioReaderMutex);
			target->audioReader.Reset();
		}
	} clearReader{ state.get() };

	reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
	if (FAILED(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE))) {
		return;
	}

	ComPtr<IMFMediaType> audioType;
	MFCreateMediaType(audioType.GetAddressOf());
	audioType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
	audioType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
	audioType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
	audioType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
	audioType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
	if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, audioType.Get()))) {
		return;
	}
	audioType.Reset();
	reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, audioType.GetAddressOf());
	WAVEFORMATEX* wave = nullptr;
	UINT32 waveSize = 0;
	if (FAILED(MFCreateWaveFormatExFromMFMediaType(audioType.Get(), &wave, &waveSize)) || !wave) {
		return;
	}
	if (!state->audio) {
		XAudio2Create(state->audio.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
	}
	if (state->audio && !state->master) {
		state->audio->CreateMasteringVoice(&state->master);
	}
	bool voiceOk = false;
	if (state->audio && state->master && !state->voice) {
		voiceOk = SUCCEEDED(state->audio->CreateSourceVoice(&state->voice, wave, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &state->events));
	}
	CoTaskMemFree(wave);
	if (!voiceOk || !state->voice) {
		return;
	}
	ApplyVolume();
	state->audible.store(true);
	state->audioReady.store(true);

	std::uint32_t seenEpoch = state->loopEpoch.load();
	state->audioEpoch.store(seenEpoch);
	bool playing = false;
	int failures = 0;

	auto restart = [&]() {
		const auto epoch = state->loopEpoch.load();
		if (epoch == seenEpoch) {
			return false;
		}
		if (state->voice) {
			state->voice->Stop();
			state->voice->FlushSourceBuffers();
		}
		playing = false;
		PROPVARIANT position{};
		position.vt = VT_I8;
		position.hVal.QuadPart = 0;
		reader->SetCurrentPosition(GUID_NULL, position);
		seenEpoch = epoch;
		state->audioEpoch.store(epoch);
		failures = 0;
		return true;
	};

	while (state->run) {
		if (playing && state->voice && !state->voiceStarted.load()) {
			state->voice->Stop();
			state->voice->FlushSourceBuffers();
			playing = false;
		}
		if (restart()) {
			continue;
		}
		if (state->voiceStarted.load() && !playing) {
			ApplyVolume();
			if (SUCCEEDED(state->voice->Start())) {
				playing = true;
			}
		}

		XAUDIO2_VOICE_STATE voiceState{};
		state->voice->GetState(&voiceState);
		if (voiceState.BuffersQueued >= 8) {
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
			continue;
		}

		DWORD flags = 0;
		LONGLONG timestamp = 0;
		ComPtr<IMFSample> sample;
		const HRESULT read = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &timestamp, sample.GetAddressOf());
		if (!state->run || restart()) {
			continue;
		}
		if (FAILED(read)) {
			if (++failures >= 8) {
				SKSE::log::warn("Чтение звука остановилось: {}", Narrow(path));
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(8));
			continue;
		}
		failures = 0;
		if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
			if (!state->loop) {
				break;
			}
			if (!state->externalAudio.empty()) {
				PROPVARIANT position{};
				position.vt = VT_I8;
				position.hVal.QuadPart = 0;
				reader->SetCurrentPosition(GUID_NULL, position);
				continue;
			}
			while (state->run && state->loopEpoch.load() == seenEpoch) {
				std::this_thread::sleep_for(std::chrono::milliseconds(4));
			}
			continue;
		}
		if (!sample) {
			continue;
		}
		ComPtr<IMFMediaBuffer> buffer;
		if (FAILED(sample->ConvertToContiguousBuffer(buffer.GetAddressOf()))) {
			continue;
		}
		BYTE* data = nullptr;
		DWORD length = 0;
		if (FAILED(buffer->Lock(&data, nullptr, &length)) || !data || length == 0) {
			continue;
		}
		auto* block = new VoiceEvents::Block();
		block->bytes.assign(data, data + length);
		buffer->Unlock();
		if (restart() || !state->run || !state->voice) {
			VoiceEvents::Release(block);
			continue;
		}
		XAUDIO2_BUFFER packet{};
		packet.AudioBytes = static_cast<UINT32>(block->bytes.size());
		packet.pAudioData = block->bytes.data();
		packet.pContext = block;
		if (FAILED(state->voice->SubmitSourceBuffer(&packet))) {
			VoiceEvents::Release(block);
		}
	}
}

namespace
{
void WriteLimitedRgba(std::uint8_t* dst, int y, int u, int v, bool hd)
{
	const int c = y - 16;
	const int d = u - 128;
	const int e = v - 128;
	const int rv = hd ? 115 : 102;
	const int gu = hd ? 14 : 25;
	const int gv = hd ? 34 : 52;
	const int bu = hd ? 135 : 129;
	const int r = (75 * c + rv * e + 32) >> 6;
	const int g = (75 * c - gu * d - gv * e + 32) >> 6;
	const int b = (75 * c + bu * d + 32) >> 6;
	auto clamp = [](int value) {
		return static_cast<std::uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
	};
	dst[0] = clamp(r);
	dst[1] = clamp(g);
	dst[2] = clamp(b);
	dst[3] = 255;
}

void ConvertFourNv12(const std::uint8_t* y4, const std::uint8_t* uv4, std::uint8_t* dst, const __m128i& rCoeff, const __m128i& gCoeff, const __m128i& gVCoeff, const __m128i& bCoeff, const __m128i& bias16, const __m128i& bias128, const __m128i& round32, const __m128i& alpha)
{
	const __m128i zero = _mm_setzero_si128();
	int yBits = 0;
	int uvBits = 0;
	std::memcpy(&yBits, y4, sizeof(yBits));
	std::memcpy(&uvBits, uv4, sizeof(uvBits));
	const __m128i y16 = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(yBits), zero), bias16);
	const __m128i uv16 = _mm_unpacklo_epi8(_mm_cvtsi32_si128(uvBits), zero);
	const __m128i u16 = _mm_sub_epi16(_mm_shufflelo_epi16(uv16, _MM_SHUFFLE(2, 2, 0, 0)), bias128);
	const __m128i v16 = _mm_sub_epi16(_mm_shufflelo_epi16(uv16, _MM_SHUFFLE(3, 3, 1, 1)), bias128);
	const __m128i r32 = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(y16, v16), rCoeff), round32), 6);
	const __m128i gCd = _mm_madd_epi16(_mm_unpacklo_epi16(y16, u16), gCoeff);
	const __m128i gE = _mm_madd_epi16(_mm_unpacklo_epi16(v16, zero), gVCoeff);
	const __m128i g32 = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(gCd, gE), round32), 6);
	const __m128i b32 = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(y16, u16), bCoeff), round32), 6);
	const __m128i r8 = _mm_packus_epi16(_mm_packs_epi32(r32, r32), zero);
	const __m128i g8 = _mm_packus_epi16(_mm_packs_epi32(g32, g32), zero);
	const __m128i b8 = _mm_packus_epi16(_mm_packs_epi32(b32, b32), zero);
	const __m128i rg = _mm_unpacklo_epi8(r8, g8);
	const __m128i ba = _mm_unpacklo_epi8(b8, alpha);
	_mm_storeu_si128(reinterpret_cast<__m128i*>(dst), _mm_unpacklo_epi16(rg, ba));
}

void ConvertNv12ToRgba(const std::uint8_t* yPlane, int yStride, const std::uint8_t* uvPlane, int uvStride, int width, int height, std::uint8_t* dst)
{
	if (!yPlane || !uvPlane || !dst || width <= 0 || height <= 0 || yStride <= 0 || uvStride <= 0) {
		return;
	}
	const bool hd = height >= 720 || width >= 1280;
	const int gv = hd ? -34 : -52;
	const int gu = hd ? -14 : -25;
	const int rv = hd ? 115 : 102;
	const int bu = hd ? 135 : 129;
	const __m128i rCoeff = _mm_set1_epi32((rv << 16) | 75);
	const __m128i gCoeff = _mm_set1_epi32((gu << 16) | 75);
	const __m128i gVCoeff = _mm_set1_epi32(gv & 0xFFFF);
	const __m128i bCoeff = _mm_set1_epi32((bu << 16) | 75);
	const __m128i bias16 = _mm_set1_epi16(16);
	const __m128i bias128 = _mm_set1_epi16(128);
	const __m128i round32 = _mm_set1_epi32(32);
	const __m128i alpha = _mm_set1_epi8(static_cast<char>(255));
	const int uvHeight = std::max(1, height / 2);
	const int simdWidth = width & ~3;
	std::vector<std::uint8_t> uvBlend(static_cast<std::size_t>(std::max(uvStride, width)));
	for (int y = 0; y < height; ++y) {
		const std::uint8_t* yRow = yPlane + static_cast<std::ptrdiff_t>(y) * yStride;
		const int cy = std::min(y / 2, uvHeight - 1);
		const std::uint8_t* uvRow = uvPlane + static_cast<std::ptrdiff_t>(cy) * uvStride;
		if ((y & 1) != 0) {
			const int cy1 = std::min(cy + 1, uvHeight - 1);
			const std::uint8_t* uvNext = uvPlane + static_cast<std::ptrdiff_t>(cy1) * uvStride;
			const int bytes = std::min(uvStride, width);
			int i = 0;
			for (; i + 16 <= bytes; i += 16) {
				const __m128i row = _mm_loadu_si128(reinterpret_cast<const __m128i*>(uvRow + i));
				const __m128i next = _mm_loadu_si128(reinterpret_cast<const __m128i*>(uvNext + i));
				_mm_storeu_si128(reinterpret_cast<__m128i*>(uvBlend.data() + i), _mm_avg_epu8(row, next));
			}
			for (; i < bytes; ++i) {
				uvBlend[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((static_cast<int>(uvRow[i]) + uvNext[i] + 1) >> 1);
			}
			uvRow = uvBlend.data();
		}
		std::uint8_t* out = dst + static_cast<std::ptrdiff_t>(y) * width * 4;
		int x = 0;
		for (; x < simdWidth; x += 4) {
			ConvertFourNv12(yRow + x, uvRow + x, out + x * 4, rCoeff, gCoeff, gVCoeff, bCoeff, bias16, bias128, round32, alpha);
		}
		for (; x < width; ++x) {
			const int cx = std::min(x / 2, std::max(0, width / 2 - 1));
			WriteLimitedRgba(out + x * 4, yRow[x], uvRow[cx * 2], uvRow[cx * 2 + 1], hd);
		}
	}
}

struct DisplaySize
{
	int fitW = 1;
	int fitH = 1;
	int turns = 0;
};

DisplaySize MeasureDisplay(IMFMediaType* nativeType, IMFMediaType* videoType, UINT frameW, UINT frameH, UINT nativeW, UINT nativeH)
{
	UINT32 parN = 1;
	UINT32 parD = 1;
	if (!nativeType || FAILED(MFGetAttributeRatio(nativeType, MF_MT_PIXEL_ASPECT_RATIO, &parN, &parD))) {
		parN = 1;
		parD = 1;
		if (videoType) {
			MFGetAttributeRatio(videoType, MF_MT_PIXEL_ASPECT_RATIO, &parN, &parD);
		}
	}
	if (parN == 0) {
		parN = 1;
	}
	if (parD == 0) {
		parD = 1;
	}
	int turns = RotationTurns(nativeType);
	if (turns == 0) {
		turns = RotationTurns(videoType);
	}
	const bool quarter = turns == 1 || turns == 3;
	if (quarter && frameW == nativeH && frameH == nativeW && nativeW != nativeH) {
		turns = 0;
	}
	UINT32 aspectW = frameW;
	UINT32 aspectH = frameH;
	if (!VisibleSize(nativeType, aspectW, aspectH)) {
		VisibleSize(videoType, aspectW, aspectH);
	}
	const double squareW = static_cast<double>(nativeW) * static_cast<double>(parN) / static_cast<double>(parD);
	const bool parAlreadyApplied = parN != parD && std::abs(static_cast<double>(frameW) - squareW) < 2.0 && frameH == nativeH;
	long long displayW = parAlreadyApplied ? static_cast<long long>(frameW) : static_cast<long long>(aspectW) * parN;
	long long displayH = parAlreadyApplied ? static_cast<long long>(frameH) : static_cast<long long>(aspectH) * parD;
	if (turns == 1 || turns == 3) {
		std::swap(displayW, displayH);
	}
	const long long divisor = Gcd(displayW, displayH);
	if (divisor > 1) {
		displayW /= divisor;
		displayH /= divisor;
	}
	DisplaySize result;
	result.fitW = static_cast<int>(std::max<long long>(1, displayW));
	result.fitH = static_cast<int>(std::max<long long>(1, displayH));
	result.turns = turns;
	return result;
}

void FittedSize(int srcW, int srcH, int screenW, int screenH, int& outW, int& outH)
{
	outW = std::max(srcW, 1);
	outH = std::max(srcH, 1);
	if (srcW < 2 || srcH < 2 || screenW < 2 || screenH < 2 || (srcW <= screenW && srcH <= screenH)) {
		return;
	}
	const double scale = std::min(static_cast<double>(screenW) / static_cast<double>(srcW), static_cast<double>(screenH) / static_cast<double>(srcH));
	int fittedW = static_cast<int>(std::lround(static_cast<double>(srcW) * scale)) & ~1;
	int fittedH = static_cast<int>(std::lround(static_cast<double>(srcH) * scale)) & ~1;
	if (fittedW > srcW) {
		fittedW = srcW & ~1;
	}
	if (fittedH > srcH) {
		fittedH = srcH & ~1;
	}
	if (fittedW < 2 || fittedH < 2) {
		return;
	}
	outW = fittedW;
	outH = fittedH;
}

void ScalePlane(const std::uint8_t* src, int srcStride, int srcW, int srcH, std::uint8_t* dst, int dstW, int dstH)
{
	for (int y = 0; y < dstH; ++y) {
		const int y0 = y * srcH / dstH;
		int y1 = (y + 1) * srcH / dstH;
		if (y1 <= y0) {
			y1 = std::min(srcH, y0 + 1);
		}
		for (int x = 0; x < dstW; ++x) {
			const int x0 = x * srcW / dstW;
			int x1 = (x + 1) * srcW / dstW;
			if (x1 <= x0) {
				x1 = std::min(srcW, x0 + 1);
			}
			std::int64_t sum = 0;
			int count = 0;
			for (int sy = y0; sy < y1; ++sy) {
				const std::uint8_t* row = src + static_cast<std::ptrdiff_t>(sy) * srcStride;
				for (int sx = x0; sx < x1; ++sx) {
					sum += row[sx];
					++count;
				}
			}
			dst[static_cast<std::size_t>(y) * dstW + x] = static_cast<std::uint8_t>(count > 0 ? sum / count : 0);
		}
	}
}

void ScaleUv(const std::uint8_t* src, int srcStride, int srcW, int srcH, std::uint8_t* dst, int dstW, int dstH)
{
	const int srcCw = std::max(1, srcW / 2);
	const int srcCh = std::max(1, srcH / 2);
	const int dstCw = std::max(1, dstW / 2);
	const int dstCh = std::max(1, dstH / 2);
	for (int y = 0; y < dstCh; ++y) {
		const int y0 = y * srcCh / dstCh;
		int y1 = (y + 1) * srcCh / dstCh;
		if (y1 <= y0) {
			y1 = std::min(srcCh, y0 + 1);
		}
		for (int x = 0; x < dstCw; ++x) {
			const int x0 = x * srcCw / dstCw;
			int x1 = (x + 1) * srcCw / dstCw;
			if (x1 <= x0) {
				x1 = std::min(srcCw, x0 + 1);
			}
			std::int64_t sumU = 0;
			std::int64_t sumV = 0;
			int count = 0;
			for (int sy = y0; sy < y1; ++sy) {
				const std::uint8_t* row = src + static_cast<std::ptrdiff_t>(sy) * srcStride;
				for (int sx = x0; sx < x1; ++sx) {
					sumU += row[sx * 2];
					sumV += row[sx * 2 + 1];
					++count;
				}
			}
			std::uint8_t* pixel = dst + (static_cast<std::size_t>(y) * dstCw + x) * 2;
			pixel[0] = static_cast<std::uint8_t>(count > 0 ? sumU / count : 128);
			pixel[1] = static_cast<std::uint8_t>(count > 0 ? sumV / count : 128);
		}
	}
}

void ScaleNv12(const std::uint8_t* ySrc, int yStride, const std::uint8_t* uvSrc, int uvStride, int srcW, int srcH, std::uint8_t* dst, int dstW, int dstH)
{
	ScalePlane(ySrc, yStride, srcW, srcH, dst, dstW, dstH);
	ScaleUv(uvSrc, uvStride, srcW, srcH, dst + static_cast<std::size_t>(dstW) * dstH, dstW, dstH);
}

void ScaleBgraToRgba(const std::uint8_t* src, int srcPitch, int srcW, int srcH, std::uint8_t* dst, int dstW, int dstH)
{
	for (int y = 0; y < dstH; ++y) {
		const int y0 = y * srcH / dstH;
		int y1 = (y + 1) * srcH / dstH;
		if (y1 <= y0) {
			y1 = std::min(srcH, y0 + 1);
		}
		for (int x = 0; x < dstW; ++x) {
			const int x0 = x * srcW / dstW;
			int x1 = (x + 1) * srcW / dstW;
			if (x1 <= x0) {
				x1 = std::min(srcW, x0 + 1);
			}
			std::int64_t sumB = 0;
			std::int64_t sumG = 0;
			std::int64_t sumR = 0;
			int count = 0;
			for (int sy = y0; sy < y1; ++sy) {
				const std::uint8_t* row = src + static_cast<std::ptrdiff_t>(sy) * srcPitch;
				for (int sx = x0; sx < x1; ++sx) {
					const std::uint8_t* pixel = row + static_cast<std::size_t>(sx) * 4;
					sumB += pixel[0];
					sumG += pixel[1];
					sumR += pixel[2];
					++count;
				}
			}
			std::uint8_t* out = dst + (static_cast<std::size_t>(y) * dstW + x) * 4;
			out[0] = static_cast<std::uint8_t>(count > 0 ? sumR / count : 0);
			out[1] = static_cast<std::uint8_t>(count > 0 ? sumG / count : 0);
			out[2] = static_cast<std::uint8_t>(count > 0 ? sumB / count : 0);
			out[3] = 255;
		}
	}
}

void ProtectDevice(ID3D11Device* device)
{
	ComPtr<ID3D11Multithread> threads;
	if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(threads.GetAddressOf())))) {
		threads->SetMultithreadProtected(TRUE);
	}
}

bool TakeDxgi(IMFSample* sample, ComPtr<ID3D11Texture2D>& texture, UINT& slice)
{
	texture.Reset();
	slice = 0;
	if (!sample) {
		return false;
	}
	ComPtr<IMFMediaBuffer> buffer;
	if (FAILED(sample->GetBufferByIndex(0, buffer.GetAddressOf())) || !buffer) {
		return false;
	}
	ComPtr<IMFDXGIBuffer> dxgi;
	if (FAILED(buffer.As(&dxgi)) || !dxgi) {
		return false;
	}
	if (FAILED(dxgi->GetResource(IID_PPV_ARGS(texture.GetAddressOf()))) || !texture) {
		return false;
	}
	dxgi->GetSubresourceIndex(&slice);
	return true;
}

}

std::vector<unsigned char> Playback::TakeSpare(std::size_t bytes)
{
	std::vector<unsigned char> buffer;
	if (state) {
		std::lock_guard lock(state->poolMutex);
		if (!state->spare.empty()) {
			buffer = std::move(state->spare.back());
			state->spare.pop_back();
		}
	}
	if (buffer.size() != bytes) {
		buffer.resize(bytes);
	}
	return buffer;
}

void Playback::Recycle(std::vector<unsigned char> buffer)
{
	if (!state || buffer.empty()) {
		return;
	}
	std::lock_guard lock(state->poolMutex);
	if (state->spare.size() < 2) {
		state->spare.push_back(std::move(buffer));
	}
}

bool Playback::EnsureVideoDevice(ID3D11Device* gameDevice)
{
	if (!gameDevice) {
		return false;
	}
	ProtectDevice(gameDevice);
	if (gpu && gpu->device && gpu->manager && gpu->paired == gameDevice) {
		return true;
	}
	gpu = std::make_unique<GpuDevice>();
	gpu->paired = gameDevice;
	ComPtr<IDXGIDevice> dxgi;
	ComPtr<IDXGIAdapter> adapter;
	if (FAILED(gameDevice->QueryInterface(IID_PPV_ARGS(dxgi.GetAddressOf()))) || FAILED(dxgi->GetAdapter(adapter.GetAddressOf()))) {
		gpu.reset();
		return false;
	}
	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	HRESULT created = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2, D3D11_SDK_VERSION, gpu->device.GetAddressOf(), nullptr, gpu->context.GetAddressOf());
	if (FAILED(created)) {
		gpu->device.Reset();
		gpu->context.Reset();
		created = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, &levels[1], 1, D3D11_SDK_VERSION, gpu->device.GetAddressOf(), nullptr, gpu->context.GetAddressOf());
	}
	if (FAILED(created) || !gpu->device || !gpu->context || FAILED(gpu->device.As(&gpu->api)) || FAILED(gpu->context.As(&gpu->videoContext))) {
		gpu.reset();
		return false;
	}
	ProtectDevice(gpu->device.Get());
	if (FAILED(MFCreateDXGIDeviceManager(&gpu->token, gpu->manager.GetAddressOf())) || FAILED(gpu->manager->ResetDevice(gpu->device.Get(), gpu->token))) {
		gpu.reset();
		return false;
	}
	return true;
}

void Playback::ReleaseGpuOutput()
{
	if (!gpu) {
		return;
	}
	gpu->outputView.Reset();
	gpu->processor.Reset();
	gpu->enumerator.Reset();
	gpu->output.Reset();
	gpu->staging.Reset();
	gpu->shared = false;
	gpu->inW = 0;
	gpu->inH = 0;
	gpu->outW = 0;
	gpu->outH = 0;
	gpu->logged = false;
}

bool Playback::EnsureGpuOutput(ID3D11Device* gameDevice, int sourceWidth, int sourceHeight, int outputWidth, int outputHeight, bool hd)
{
	if (!gpu || !gpu->device || !gpu->api || !gameDevice || sourceWidth < 2 || sourceHeight < 2 || outputWidth < 2 || outputHeight < 2) {
		return false;
	}
	ReleaseGpuOutput();
	texture.Reset();
	view.Reset();
	textureDynamic = false;

	auto makeOutput = [&](UINT misc) {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = static_cast<UINT>(outputWidth);
		desc.Height = static_cast<UINT>(outputHeight);
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = misc;
		gpu->output.Reset();
		return SUCCEEDED(gpu->device->CreateTexture2D(&desc, nullptr, gpu->output.GetAddressOf()));
	};

	if (makeOutput(D3D11_RESOURCE_MISC_SHARED)) {
		ComPtr<IDXGIResource> resource;
		HANDLE handle = nullptr;
		if (SUCCEEDED(gpu->output.As(&resource)) && SUCCEEDED(resource->GetSharedHandle(&handle)) && handle && SUCCEEDED(gameDevice->OpenSharedResource(handle, IID_PPV_ARGS(texture.GetAddressOf())))) {
			gpu->shared = true;
		}
	}
	if (!gpu->shared) {
		texture.Reset();
		gpu->output.Reset();
		if (!makeOutput(0)) {
			ReleaseGpuOutput();
			return false;
		}
		D3D11_TEXTURE2D_DESC stage{};
		stage.Width = static_cast<UINT>(outputWidth);
		stage.Height = static_cast<UINT>(outputHeight);
		stage.MipLevels = 1;
		stage.ArraySize = 1;
		stage.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		stage.SampleDesc.Count = 1;
		stage.Usage = D3D11_USAGE_STAGING;
		stage.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(gpu->device->CreateTexture2D(&stage, nullptr, gpu->staging.GetAddressOf()))) {
			ReleaseGpuOutput();
			return false;
		}
		D3D11_TEXTURE2D_DESC gameDesc{};
		gameDesc.Width = static_cast<UINT>(outputWidth);
		gameDesc.Height = static_cast<UINT>(outputHeight);
		gameDesc.MipLevels = 1;
		gameDesc.ArraySize = 1;
		gameDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		gameDesc.SampleDesc.Count = 1;
		gameDesc.Usage = D3D11_USAGE_DYNAMIC;
		gameDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		gameDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(gameDevice->CreateTexture2D(&gameDesc, nullptr, texture.GetAddressOf()))) {
			ReleaseGpuOutput();
			texture.Reset();
			return false;
		}
		textureDynamic = true;
	}
	if (FAILED(gameDevice->CreateShaderResourceView(texture.Get(), nullptr, view.GetAddressOf()))) {
		ReleaseGpuOutput();
		texture.Reset();
		view.Reset();
		return false;
	}

	D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
	content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	content.InputFrameRate.Numerator = 60;
	content.InputFrameRate.Denominator = 1;
	content.InputWidth = static_cast<UINT>(sourceWidth);
	content.InputHeight = static_cast<UINT>(sourceHeight);
	content.OutputFrameRate.Numerator = 60;
	content.OutputFrameRate.Denominator = 1;
	content.OutputWidth = static_cast<UINT>(outputWidth);
	content.OutputHeight = static_cast<UINT>(outputHeight);
	content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
	if (FAILED(gpu->api->CreateVideoProcessorEnumerator(&content, gpu->enumerator.GetAddressOf()))) {
		ReleaseGpuOutput();
		texture.Reset();
		view.Reset();
		return false;
	}
	UINT support = 0;
	if (FAILED(gpu->enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support)) || (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0) {
		ReleaseGpuOutput();
		texture.Reset();
		view.Reset();
		return false;
	}
	support = 0;
	if (FAILED(gpu->enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &support)) || (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
		ReleaseGpuOutput();
		texture.Reset();
		view.Reset();
		return false;
	}
	if (FAILED(gpu->api->CreateVideoProcessor(gpu->enumerator.Get(), 0, gpu->processor.GetAddressOf()))) {
		ReleaseGpuOutput();
		texture.Reset();
		view.Reset();
		return false;
	}

	D3D11_VIDEO_PROCESSOR_COLOR_SPACE input{};
	input.Usage = 0;
	input.RGB_Range = 1;
	input.YCbCr_Matrix = hd ? 1u : 0u;
	input.YCbCr_xvYCC = 0;
	input.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
	D3D11_VIDEO_PROCESSOR_COLOR_SPACE output{};
	output.Usage = 0;
	output.RGB_Range = 0;
	output.YCbCr_Matrix = hd ? 1u : 0u;
	output.YCbCr_xvYCC = 0;
	output.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
	gpu->videoContext->VideoProcessorSetStreamColorSpace(gpu->processor.Get(), 0, &input);
	gpu->videoContext->VideoProcessorSetOutputColorSpace(gpu->processor.Get(), &output);
	gpu->videoContext->VideoProcessorSetStreamAutoProcessingMode(gpu->processor.Get(), 0, FALSE);
	gpu->videoContext->VideoProcessorSetStreamFrameFormat(gpu->processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

	D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
	outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
	outputDesc.Texture2D.MipSlice = 0;
	if (FAILED(gpu->api->CreateVideoProcessorOutputView(gpu->output.Get(), gpu->enumerator.Get(), &outputDesc, gpu->outputView.GetAddressOf()))) {
		ReleaseGpuOutput();
		texture.Reset();
		view.Reset();
		return false;
	}

	gpu->inW = sourceWidth;
	gpu->inH = sourceHeight;
	gpu->outW = outputWidth;
	gpu->outH = outputHeight;
	gpu->hd = hd;
	width = outputWidth;
	height = outputHeight;
	if (!gpu->logged) {
		gpu->logged = true;
		SKSE::log::info("Видеопроцессор {} {}x{} -> {}x{}", state ? state->label : std::string{}, sourceWidth, sourceHeight, outputWidth, outputHeight);
	}
	return true;
}

bool Playback::BlitGpu(ID3D11Device* gameDevice, ID3D11DeviceContext* gameContext, int screenWidth, int screenHeight, const std::shared_ptr<GpuFrame>& frame)
{
	if (!gpu || !gpu->api || !gpu->videoContext || !frame || !frame->texture || frame->width < 2 || frame->height < 2) {
		return false;
	}
	D3D11_TEXTURE2D_DESC srcDesc{};
	frame->texture->GetDesc(&srcDesc);
	int outW = 0;
	int outH = 0;
	FittedSize(frame->width, frame->height, screenWidth, screenHeight, outW, outH);
	const bool hd = frame->height >= 720 || frame->width >= 1280;
	const int srcW = static_cast<int>(srcDesc.Width);
	const int srcH = static_cast<int>(srcDesc.Height);
	if (!gpu->processor || gpu->inW != srcW || gpu->inH != srcH || gpu->outW != outW || gpu->outH != outH || gpu->hd != hd || !view) {
		if (!EnsureGpuOutput(gameDevice, srcW, srcH, outW, outH, hd)) {
			if (gpu && !gpu->blitLogged) {
				gpu->blitLogged = true;
				SKSE::log::error("Видеопроцессор не создался");
			}
			return false;
		}
	}

	D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
	inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
	inputDesc.Texture2D.MipSlice = 0;
	inputDesc.Texture2D.ArraySlice = srcDesc.ArraySize > 1 ? frame->slice : 0;
	ComPtr<ID3D11VideoProcessorInputView> inputView;
	if (FAILED(gpu->api->CreateVideoProcessorInputView(frame->texture.Get(), gpu->enumerator.Get(), &inputDesc, inputView.GetAddressOf()))) {
		if (!gpu->blitLogged) {
			gpu->blitLogged = true;
			SKSE::log::error("Вход видеопроцессора не создался");
		}
		return false;
	}

	RECT source{ 0, 0, frame->width, frame->height };
	RECT dest{ 0, 0, gpu->outW, gpu->outH };
	gpu->videoContext->VideoProcessorSetStreamSourceRect(gpu->processor.Get(), 0, TRUE, &source);
	gpu->videoContext->VideoProcessorSetStreamDestRect(gpu->processor.Get(), 0, TRUE, &dest);
	gpu->videoContext->VideoProcessorSetOutputTargetRect(gpu->processor.Get(), TRUE, &dest);

	D3D11_VIDEO_PROCESSOR_STREAM stream{};
	stream.Enable = TRUE;
	stream.pInputSurface = inputView.Get();
	if (FAILED(gpu->videoContext->VideoProcessorBlt(gpu->processor.Get(), gpu->outputView.Get(), 0, 1, &stream))) {
		if (!gpu->blitLogged) {
			gpu->blitLogged = true;
			SKSE::log::error("Видеопроцессор не нарисовал кадр");
		}
		return false;
	}
	gpu->context->Flush();
	if (!gpu->shared) {
		if (!gpu->staging || !texture || !gameContext) {
			return false;
		}
		gpu->context->CopyResource(gpu->staging.Get(), gpu->output.Get());
		D3D11_MAPPED_SUBRESOURCE read{};
		if (FAILED(gpu->context->Map(gpu->staging.Get(), 0, D3D11_MAP_READ, 0, &read))) {
			return false;
		}
		D3D11_MAPPED_SUBRESOURCE write{};
		if (FAILED(gameContext->Map(texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &write))) {
			gpu->context->Unmap(gpu->staging.Get(), 0);
			return false;
		}
		const UINT rowBytes = static_cast<UINT>(gpu->outW) * 4;
		for (int y = 0; y < gpu->outH; ++y) {
			std::memcpy(static_cast<unsigned char*>(write.pData) + static_cast<std::size_t>(y) * write.RowPitch, static_cast<const unsigned char*>(read.pData) + static_cast<std::size_t>(y) * read.RowPitch, rowBytes);
		}
		gameContext->Unmap(texture.Get(), 0);
		gpu->context->Unmap(gpu->staging.Get(), 0);
	}
	width = gpu->outW;
	height = gpu->outH;
	return true;
}

bool Playback::RunHardware(const std::filesystem::path& path)
{
	if (!gpu || !gpu->manager || !gpu->device) {
		return false;
	}
	HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	struct CoGuard
	{
		bool own = false;
		~CoGuard()
		{
			if (own) {
				CoUninitialize();
			}
		}
	} guard{ SUCCEEDED(com) };

	ComPtr<IMFAttributes> attributes;
	if (FAILED(MFCreateAttributes(attributes.GetAddressOf(), 3))) {
		return false;
	}
	attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);
	attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
	if (FAILED(attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, gpu->manager.Get()))) {
		return false;
	}

	ComPtr<IMFSourceReader> reader;
	if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), reader.GetAddressOf()))) {
		return false;
	}
	{
		std::lock_guard lock(state->readerMutex);
		state->reader = reader;
	}
	struct ReaderClear
	{
		State* target = nullptr;
		~ReaderClear()
		{
			if (!target) {
				return;
			}
			std::lock_guard lock(target->readerMutex);
			target->reader.Reset();
		}
	} clearReader{ state.get() };

	reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
	reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

	ComPtr<IMFMediaType> nativeType;
	reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nativeType.GetAddressOf());
	UINT32 nativeW = 0;
	UINT32 nativeH = 0;
	UINT32 fpsN = 0;
	UINT32 fpsD = 0;
	if (nativeType) {
		MFGetAttributeSize(nativeType.Get(), MF_MT_FRAME_SIZE, &nativeW, &nativeH);
		MFGetAttributeRatio(nativeType.Get(), MF_MT_FRAME_RATE, &fpsN, &fpsD);
	}
	auto useOutput = [&](bool pinSize) {
		ComPtr<IMFMediaType> wanted;
		if (FAILED(MFCreateMediaType(wanted.GetAddressOf()))) {
			return E_OUTOFMEMORY;
		}
		wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		wanted->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
		if (pinSize && nativeW > 0 && nativeH > 0) {
			MFSetAttributeSize(wanted.Get(), MF_MT_FRAME_SIZE, nativeW, nativeH);
			if (fpsN > 0 && fpsD > 0) {
				MFSetAttributeRatio(wanted.Get(), MF_MT_FRAME_RATE, fpsN, fpsD);
			}
			wanted->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		}
		return reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, wanted.Get());
	};
	if (FAILED(useOutput(true)) && FAILED(useOutput(false))) {
		SKSE::log::info("DXVA NV12 недоступен, запасной путь: {}", Narrow(path));
		return false;
	}

	ComPtr<IMFMediaType> videoType;
	if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, videoType.GetAddressOf())) || !videoType) {
		return false;
	}
	UINT32 frameW = 0;
	UINT32 frameH = 0;
	MFGetAttributeSize(videoType.Get(), MF_MT_FRAME_SIZE, &frameW, &frameH);
	if (frameW == 0 || frameH == 0) {
		return false;
	}
	if (nativeW == 0 || nativeH == 0) {
		nativeW = frameW;
		nativeH = frameH;
	}
	UINT32 pictureW = nativeW;
	UINT32 pictureH = nativeH;
	auto refreshVideoType = [&]() {
		ComPtr<IMFMediaType> current;
		if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, current.GetAddressOf()))) {
			return;
		}
		UINT32 newW = 0;
		UINT32 newH = 0;
		if (SUCCEEDED(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &newW, &newH)) && newW > 0 && newH > 0) {
			frameW = newW;
			frameH = newH;
		}
	};
	const DisplaySize display = MeasureDisplay(nativeType.Get(), videoType.Get(), frameW, frameH, nativeW, nativeH);
	fitWidth = display.fitW;
	fitHeight = display.fitH;
	orientation = display.turns;
	D3D11_VIDEO_PROCESSOR_CONTENT_DESC probeDesc{};
	probeDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	probeDesc.InputFrameRate.Numerator = fpsN > 0 ? fpsN : 30;
	probeDesc.InputFrameRate.Denominator = fpsD > 0 ? fpsD : 1;
	probeDesc.InputWidth = frameW;
	probeDesc.InputHeight = frameH;
	probeDesc.OutputFrameRate = probeDesc.InputFrameRate;
	probeDesc.OutputWidth = frameW;
	probeDesc.OutputHeight = frameH;
	probeDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
	ComPtr<ID3D11VideoProcessorEnumerator> probeEnum;
	ComPtr<ID3D11VideoProcessor> probeProcessor;
	UINT probeSupport = 0;
	if (!gpu->api || FAILED(gpu->api->CreateVideoProcessorEnumerator(&probeDesc, probeEnum.GetAddressOf())) || FAILED(probeEnum->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &probeSupport)) || (probeSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0) {
		SKSE::log::info("Видеопроцессор недоступен, запасной путь: {}", Narrow(path));
		return false;
	}
	probeSupport = 0;
	if (FAILED(probeEnum->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &probeSupport)) || (probeSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0 || FAILED(gpu->api->CreateVideoProcessor(probeEnum.Get(), 0, probeProcessor.GetAddressOf()))) {
		SKSE::log::info("Видеопроцессор недоступен, запасной путь: {}", Narrow(path));
		return false;
	}
	probeProcessor.Reset();
	probeEnum.Reset();
	SKSE::log::info("Видео {} dxva кадр {}x{} показ {}x{} поворот {}", Narrow(path), frameW, frameH, fitWidth, fitHeight, display.turns * 90);

	std::deque<std::pair<LONGLONG, std::shared_ptr<GpuFrame>>> queue;
	auto accept = [&](IMFSample* sample, LONGLONG timestamp) {
		ComPtr<ID3D11Texture2D> tex;
		UINT slice = 0;
		if (!TakeDxgi(sample, tex, slice)) {
			return -1;
		}
		D3D11_TEXTURE2D_DESC desc{};
		tex->GetDesc(&desc);
		if (desc.Format != DXGI_FORMAT_NV12) {
			return -1;
		}
		UINT copyW = frameW;
		UINT copyH = frameH;
		if (pictureW > 0 && pictureH > 0 && frameW >= pictureW && frameH >= pictureH && frameW - pictureW <= 16 && frameH - pictureH <= 16) {
			copyW = pictureW;
			copyH = pictureH;
		}
		const int visibleW = static_cast<int>(std::min<UINT>(copyW, desc.Width)) & ~1;
		const int visibleH = static_cast<int>(std::min<UINT>(copyH, desc.Height)) & ~1;
		if (visibleW < 2 || visibleH < 2) {
			return 0;
		}
		auto frame = std::make_shared<GpuFrame>();
		frame->sample = sample;
		frame->texture = tex;
		frame->slice = desc.ArraySize > 1 ? slice : 0;
		frame->width = visibleW;
		frame->height = visibleH;
		queue.emplace_back(timestamp, std::move(frame));
		return 1;
	};

	bool primed = false;
	for (int attempt = 0; attempt < 32 && state->run && !primed; ++attempt) {
		DWORD stream = 0;
		DWORD flags = 0;
		LONGLONG timestamp = 0;
		ComPtr<IMFSample> sample;
		const HRESULT read = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &timestamp, sample.GetAddressOf());
		if (FAILED(read) || !state->run) {
			return false;
		}
		if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
			refreshVideoType();
		}
		if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
			return false;
		}
		if (!sample) {
			continue;
		}
		const int accepted = accept(sample.Get(), timestamp);
		if (accepted < 0) {
			SKSE::log::info("Кадр без DXVA, запасной путь: {}", Narrow(path));
			return false;
		}
		primed = accepted > 0;
	}
	if (!primed) {
		return false;
	}

	LARGE_INTEGER frequency{};
	LARGE_INTEGER origin{};
	QueryPerformanceFrequency(&frequency);
	bool clock = false;
	LONGLONG originPts = 0;
	bool first = true;
	auto publishDue = [&]() {
		std::shared_ptr<GpuFrame> due;
		{
			if (queue.empty()) {
				return;
			}
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			if (!clock) {
				clock = true;
				origin = now;
				originPts = queue.front().first;
				state->voiceStarted.store(true);
			}
			const double elapsedMs = frequency.QuadPart > 0 ? (static_cast<double>(now.QuadPart - origin.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart)) : 0.0;
			double dueMs = static_cast<double>(queue.front().first - originPts) / 10000.0;
			if (elapsedMs + 8.0 < dueMs) {
				return;
			}
			if (queue.size() > 1 && elapsedMs > dueMs + 500.0) {
				queue.pop_front();
				dueMs = static_cast<double>(queue.front().first - originPts) / 10000.0;
				if (elapsedMs + 8.0 < dueMs) {
					return;
				}
			}
			if (elapsedMs > dueMs + 12.0 && frequency.QuadPart > 0) {
				const double lateMs = elapsedMs - dueMs;
				origin.QuadPart += static_cast<LONGLONG>(std::llround(lateMs * static_cast<double>(frequency.QuadPart) / 1000.0));
			}
			due = queue.front().second;
			queue.pop_front();
		}
		std::lock_guard lock(state->mutex);
		due->serial = state->gpuFrame ? state->gpuFrame->serial + 1 : 1;
		state->gpuFrame = due;
		if (first) {
			state->opened = true;
			state->ready.notify_all();
			first = false;
		}
	};
	auto restartAudio = [&]() {
		if (!state->audioReady.load()) {
			return;
		}
		state->voiceStarted.store(false);
		const auto next = state->loopEpoch.load() + 1;
		state->loopEpoch.store(next);
		ComPtr<IMFSourceReader> pendingAudio;
		{
			std::lock_guard lock(state->audioReaderMutex);
			pendingAudio = state->audioReader;
		}
		if (pendingAudio) {
			pendingAudio->Flush(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
		}
		const auto waitStart = std::chrono::steady_clock::now();
		while (state->run && state->audioEpoch.load() != next) {
			if (std::chrono::steady_clock::now() - waitStart > std::chrono::milliseconds(500)) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	};

	while (state->run) {
		publishDue();
		int shownFrames = 0;
		{
			std::lock_guard lock(state->mutex);
			shownFrames = state->gpuFrame ? 1 : 0;
		}
		if (static_cast<int>(queue.size()) + shownFrames >= 2) {
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
			continue;
		}

		DWORD stream = 0;
		DWORD flags = 0;
		LONGLONG timestamp = 0;
		ComPtr<IMFSample> sample;
		const HRESULT read = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &timestamp, sample.GetAddressOf());
		if (FAILED(read) || !state->run) {
			break;
		}
		if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
			refreshVideoType();
		}
		if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
			if (!state->loop) {
				while (state->run && !queue.empty()) {
					publishDue();
					std::this_thread::sleep_for(std::chrono::milliseconds(4));
				}
				if (state->run) {
					state->finished = true;
				}
				break;
			}
			restartAudio();
			queue.clear();
			clock = false;
			PROPVARIANT position{};
			position.vt = VT_I8;
			position.hVal.QuadPart = 0;
			reader->SetCurrentPosition(GUID_NULL, position);
			continue;
		}
		if (!sample) {
			continue;
		}
		if (accept(sample.Get(), timestamp) < 0) {
			SKSE::log::warn("DXVA кадр пропал: {}", Narrow(path));
			break;
		}
	}

	return !first;
}

void Playback::VideoMain(std::filesystem::path path)
{
	if (!RunHardware(path) && state && state->run && !state->opened && !state->failed) {
		RunSoftware(path);
	}
}

void Playback::RunSoftware(std::filesystem::path path)
{
	HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool ownCom = SUCCEEDED(com);
	state->dynamicUpload.store(true);

	auto fail = [this]() {
		std::lock_guard lock(state->mutex);
		state->failed = true;
		state->ready.notify_all();
	};

	ComPtr<IMFAttributes> attributes;
	if (FAILED(MFCreateAttributes(attributes.GetAddressOf(), 3))) {
		fail();
		if (ownCom) {
			CoUninitialize();
		}
		return;
	}
	attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);
	attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
	attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE);

	ComPtr<IMFSourceReader> reader;
	if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), reader.GetAddressOf()))) {
		SKSE::log::error("Видео не открылось: {}", Narrow(path));
		fail();
		if (ownCom) {
			CoUninitialize();
		}
		return;
	}
	{
		std::lock_guard lock(state->readerMutex);
		state->reader = reader;
	}
	struct ReaderClear
	{
		State* target = nullptr;
		~ReaderClear()
		{
			if (!target) {
				return;
			}
			std::lock_guard lock(target->readerMutex);
			target->reader.Reset();
		}
	} clearReader{ state.get() };

	reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
	reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

	ComPtr<IMFMediaType> nativeType;
	reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nativeType.GetAddressOf());
	UINT32 nativeW = 0;
	UINT32 nativeH = 0;
	UINT32 fpsN = 0;
	UINT32 fpsD = 0;
	if (nativeType) {
		MFGetAttributeSize(nativeType.Get(), MF_MT_FRAME_SIZE, &nativeW, &nativeH);
		MFGetAttributeRatio(nativeType.Get(), MF_MT_FRAME_RATE, &fpsN, &fpsD);
	}
	auto useOutput = [&](const GUID& subtype, bool pinSize) {
		ComPtr<IMFMediaType> wanted;
		if (FAILED(MFCreateMediaType(wanted.GetAddressOf()))) {
			return E_OUTOFMEMORY;
		}
		wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		wanted->SetGUID(MF_MT_SUBTYPE, subtype);
		if (pinSize && nativeW > 0 && nativeH > 0) {
			MFSetAttributeSize(wanted.Get(), MF_MT_FRAME_SIZE, nativeW, nativeH);
			if (fpsN > 0 && fpsD > 0) {
				MFSetAttributeRatio(wanted.Get(), MF_MT_FRAME_RATE, fpsN, fpsD);
			}
			wanted->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		}
		return reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, wanted.Get());
	};
	bool nv12 = SUCCEEDED(useOutput(MFVideoFormat_NV12, true)) || SUCCEEDED(useOutput(MFVideoFormat_NV12, false));
	if (!nv12) {
		reader.Reset();
		attributes.Reset();
		if (FAILED(MFCreateAttributes(attributes.GetAddressOf(), 3))) {
			fail();
			if (ownCom) {
				CoUninitialize();
			}
			return;
		}
		attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
		attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
		attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE);
		if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), reader.GetAddressOf()))) {
			SKSE::log::error("Видео не открылось: {}", Narrow(path));
			fail();
			if (ownCom) {
				CoUninitialize();
			}
			return;
		}
		{
			std::lock_guard lock(state->readerMutex);
			state->reader = reader;
		}
		reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
		reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
		nativeType.Reset();
		reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nativeType.GetAddressOf());
		if (FAILED(useOutput(MFVideoFormat_RGB32, false))) {
			SKSE::log::error("Видео без поддерживаемого кадра: {}", Narrow(path));
			fail();
			if (ownCom) {
				CoUninitialize();
			}
			return;
		}
	}

	ComPtr<IMFMediaType> videoType;
	if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, videoType.GetAddressOf())) || !videoType) {
		fail();
		if (ownCom) {
			CoUninitialize();
		}
		return;
	}
	UINT32 frameW = 0;
	UINT32 frameH = 0;
	MFGetAttributeSize(videoType.Get(), MF_MT_FRAME_SIZE, &frameW, &frameH);
	if (frameW == 0 || frameH == 0) {
		fail();
		if (ownCom) {
			CoUninitialize();
		}
		return;
	}

	if (nativeW == 0 || nativeH == 0) {
		nativeW = frameW;
		nativeH = frameH;
	}
	GUID outputSubtype{};
	if (videoType) {
		videoType->GetGUID(MF_MT_SUBTYPE, &outputSubtype);
	}
	nv12 = outputSubtype == MFVideoFormat_NV12;
	UINT32 parN = 1;
	UINT32 parD = 1;
	if (!nativeType || FAILED(MFGetAttributeRatio(nativeType.Get(), MF_MT_PIXEL_ASPECT_RATIO, &parN, &parD))) {
		parN = 1;
		parD = 1;
		MFGetAttributeRatio(videoType.Get(), MF_MT_PIXEL_ASPECT_RATIO, &parN, &parD);
	}
	if (parN == 0) {
		parN = 1;
	}
	if (parD == 0) {
		parD = 1;
	}
	LONG sourceStride = nv12 ? static_cast<LONG>(frameW) : static_cast<LONG>(frameW) * 4;
	UINT32 strideBits = 0;
	if (SUCCEEDED(videoType->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideBits))) {
		sourceStride = static_cast<LONG>(strideBits);
	}
	const UINT32 pictureW = nativeW > 0 ? nativeW : frameW;
	const UINT32 pictureH = nativeH > 0 ? nativeH : frameH;
	auto refreshVideoType = [&]() {
		ComPtr<IMFMediaType> current;
		if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, current.GetAddressOf()))) {
			return;
		}
		UINT32 newW = 0;
		UINT32 newH = 0;
		if (SUCCEEDED(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &newW, &newH)) && newW > 0 && newH > 0) {
			frameW = newW;
			frameH = newH;
		}
		GUID currentSubtype{};
		current->GetGUID(MF_MT_SUBTYPE, &currentSubtype);
		nv12 = currentSubtype == MFVideoFormat_NV12;
		UINT32 rawStride = 0;
		if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawStride))) {
			sourceStride = static_cast<LONG>(rawStride);
		} else {
			sourceStride = nv12 ? static_cast<LONG>(frameW) : static_cast<LONG>(frameW) * 4;
		}
	};

	int turns = RotationTurns(nativeType.Get());
	if (turns == 0) {
		turns = RotationTurns(videoType.Get());
	}
	const bool quarter = turns == 1 || turns == 3;
	if (quarter && frameW == nativeH && frameH == nativeW && nativeW != nativeH) {
		turns = 0;
	}
	UINT32 aspectW = frameW;
	UINT32 aspectH = frameH;
	if (!VisibleSize(nativeType.Get(), aspectW, aspectH)) {
		VisibleSize(videoType.Get(), aspectW, aspectH);
	}
	const double squareW = static_cast<double>(nativeW) * static_cast<double>(parN) / static_cast<double>(parD);
	const bool parAlreadyApplied = parN != parD && std::abs(static_cast<double>(frameW) - squareW) < 2.0 && frameH == nativeH;
	long long displayW = parAlreadyApplied ? static_cast<long long>(frameW) : static_cast<long long>(aspectW) * parN;
	long long displayH = parAlreadyApplied ? static_cast<long long>(frameH) : static_cast<long long>(aspectH) * parD;
	if (turns == 1 || turns == 3) {
		std::swap(displayW, displayH);
	}
	const long long divisor = Gcd(displayW, displayH);
	if (divisor > 1) {
		displayW /= divisor;
		displayH /= divisor;
	}
	fitWidth = static_cast<int>(std::max<long long>(1, displayW));
	fitHeight = static_cast<int>(std::max<long long>(1, displayH));
	orientation = turns;
	SKSE::log::info(
		"Видео {} cpu кадр {}x{} {} показ {}x{} поворот {}",
		Narrow(path),
		frameW,
		frameH,
		nv12 ? "nv12" : "rgb32",
		fitWidth,
		fitHeight,
		turns * 90);

	bool loggedScale = false;
	std::vector<unsigned char> scaledNv12;
	std::deque<std::pair<LONGLONG, std::shared_ptr<SharedFrame>>> queue;
	LARGE_INTEGER frequency{};
	LARGE_INTEGER origin{};
	QueryPerformanceFrequency(&frequency);
	bool clock = false;
	LONGLONG originPts = 0;
	bool first = true;

	auto publishDue = [&]() {
		std::shared_ptr<SharedFrame> due;
		{
			if (queue.empty()) {
				return;
			}
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			if (!clock) {
				clock = true;
				origin = now;
				originPts = queue.front().first;
				state->voiceStarted.store(true);
			}
			const double elapsedMs = frequency.QuadPart > 0 ? (static_cast<double>(now.QuadPart - origin.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart)) : 0.0;
			double dueMs = static_cast<double>(queue.front().first - originPts) / 10000.0;
			if (elapsedMs + 8.0 < dueMs) {
				return;
			}
			if (queue.size() > 1 && elapsedMs > dueMs + 500.0) {
				queue.pop_front();
				dueMs = static_cast<double>(queue.front().first - originPts) / 10000.0;
				if (elapsedMs + 8.0 < dueMs) {
					return;
				}
			}
			if (elapsedMs > dueMs + 12.0 && frequency.QuadPart > 0) {
				const double lateMs = elapsedMs - dueMs;
				origin.QuadPart += static_cast<LONGLONG>(std::llround(lateMs * static_cast<double>(frequency.QuadPart) / 1000.0));
			}
			due = queue.front().second;
			queue.pop_front();
		}
		std::lock_guard lock(state->mutex);
		due->serial = state->frame ? state->frame->serial + 1 : 1;
		state->frame = due;
		if (first) {
			state->opened = true;
			state->ready.notify_all();
			first = false;
		}
	};

	auto restartAudio = [&]() {
		if (!state->audioReady.load()) {
			return;
		}
		state->voiceStarted.store(false);
		const auto next = state->loopEpoch.load() + 1;
		state->loopEpoch.store(next);
		ComPtr<IMFSourceReader> pendingAudio;
		{
			std::lock_guard lock(state->audioReaderMutex);
			pendingAudio = state->audioReader;
		}
		if (pendingAudio) {
			pendingAudio->Flush(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
		}
		const auto waitStart = std::chrono::steady_clock::now();
		while (state->run && state->audioEpoch.load() != next) {
			if (std::chrono::steady_clock::now() - waitStart > std::chrono::milliseconds(500)) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	};

	while (state->run) {
		publishDue();
		if (queue.size() >= 4) {
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
			continue;
		}

		DWORD stream = 0;
		DWORD flags = 0;
		LONGLONG timestamp = 0;
		ComPtr<IMFSample> sample;
		const HRESULT read = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &timestamp, sample.GetAddressOf());
		if (FAILED(read) || !state->run) {
			if (FAILED(read) && state->run) {
				SKSE::log::error("Чтение видео остановилось: {}", Narrow(path));
			}
			break;
		}
		if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
			refreshVideoType();
		}
		if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
			if (!state->loop) {
				while (state->run && !queue.empty()) {
					publishDue();
					std::this_thread::sleep_for(std::chrono::milliseconds(4));
				}
				if (state->run) {
					state->finished = true;
				}
				break;
			}
			restartAudio();
			queue.clear();
			clock = false;
			PROPVARIANT position{};
			position.vt = VT_I8;
			position.hVal.QuadPart = 0;
			reader->SetCurrentPosition(GUID_NULL, position);
			continue;
		}
		if (!sample) {
			continue;
		}

		ComPtr<IMFMediaBuffer> buffer;
		ComPtr<IMF2DBuffer> planar;
		BYTE* scan = nullptr;
		LONG stride = sourceStride;
		DWORD length = 0;
		bool locked2d = false;
		if (FAILED(sample->GetBufferByIndex(0, buffer.GetAddressOf()))) {
			continue;
		}
		if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(planar.GetAddressOf()))) && SUCCEEDED(planar->Lock2D(&scan, &stride))) {
			locked2d = true;
		} else {
			buffer.Reset();
			planar.Reset();
			if (FAILED(sample->ConvertToContiguousBuffer(buffer.GetAddressOf())) || FAILED(buffer->Lock(&scan, nullptr, &length))) {
				continue;
			}
			if (!nv12 && frameH > 0 && length >= frameH && length % frameH == 0) {
				const LONG packed = static_cast<LONG>(length / frameH);
				if (packed >= static_cast<LONG>(frameW) * 4) {
					stride = stride < 0 ? -packed : packed;
				}
			}
		}
		UINT32 copyW = frameW;
		UINT32 copyH = frameH;
		if (pictureW > 0 && pictureH > 0 && frameW >= pictureW && frameH >= pictureH && frameW - pictureW <= 16 && frameH - pictureH <= 16) {
			copyW = pictureW;
			copyH = pictureH;
		}
		const LONG pitch = std::abs(stride);
		const BYTE* top = stride < 0 ? scan + static_cast<std::ptrdiff_t>(frameH - 1) * stride : scan;
		const bool nv12Inside = locked2d || length == 0 || length >= static_cast<DWORD>(static_cast<std::size_t>(pitch) * frameH * 3 / 2);
		const bool bgraInside = locked2d || length == 0 || (static_cast<std::size_t>(pitch) * (copyH > 0 ? copyH - 1 : 0) + static_cast<std::size_t>(copyW) * 4) <= length;
		const bool rowFits = nv12 ? pitch >= static_cast<LONG>(copyW) && nv12Inside : pitch >= static_cast<LONG>(copyW) * 4 && bgraInside;
		if (!scan || copyW == 0 || copyH == 0 || !rowFits) {
			if (locked2d) {
				planar->Unlock2D();
			} else if (buffer) {
				buffer->Unlock();
			}
			continue;
		}
		int outW = static_cast<int>(copyW);
		int outH = static_cast<int>(copyH);
		FittedSize(static_cast<int>(copyW), static_cast<int>(copyH), state->targetW.load(), state->targetH.load(), outW, outH);
		if (!loggedScale && (outW != static_cast<int>(copyW) || outH != static_cast<int>(copyH))) {
			loggedScale = true;
			SKSE::log::info("Видео {} cpu масштаб {}x{} -> {}x{}", Narrow(path), copyW, copyH, outW, outH);
		}
		auto rgba = TakeSpare(static_cast<std::size_t>(outW) * static_cast<std::size_t>(outH) * 4);
		if (nv12) {
			const BYTE* uvPlane = top + static_cast<std::ptrdiff_t>(pitch) * frameH;
			if (outW == static_cast<int>(copyW) && outH == static_cast<int>(copyH)) {
				ConvertNv12ToRgba(top, pitch, uvPlane, pitch, outW, outH, rgba.data());
			} else {
				scaledNv12.resize(static_cast<std::size_t>(outW) * static_cast<std::size_t>(outH) * 3 / 2);
				ScaleNv12(top, pitch, uvPlane, pitch, static_cast<int>(copyW), static_cast<int>(copyH), scaledNv12.data(), outW, outH);
				ConvertNv12ToRgba(scaledNv12.data(), outW, scaledNv12.data() + static_cast<std::size_t>(outW) * outH, outW, outW, outH, rgba.data());
			}
		} else if (outW == static_cast<int>(copyW) && outH == static_cast<int>(copyH)) {
			for (UINT32 y = 0; y < copyH; ++y) {
				const BYTE* row = top + static_cast<std::ptrdiff_t>(y) * pitch;
				auto* dst = reinterpret_cast<std::uint32_t*>(rgba.data() + static_cast<std::size_t>(y) * copyW * 4);
				const auto* src = reinterpret_cast<const std::uint32_t*>(row);
				for (UINT32 x = 0; x < copyW; ++x) {
					const std::uint32_t bgra = src[x];
					dst[x] = ((bgra & 0x00FF0000u) >> 16) | (bgra & 0x0000FF00u) | ((bgra & 0x000000FFu) << 16) | 0xFF000000u;
				}
			}
		} else {
			ScaleBgraToRgba(top, pitch, static_cast<int>(copyW), static_cast<int>(copyH), rgba.data(), outW, outH);
		}
		if (locked2d) {
			planar->Unlock2D();
		} else {
			buffer->Unlock();
		}
		auto frame = std::make_shared<SharedFrame>();
		frame->rgba = std::move(rgba);
		frame->width = outW;
		frame->height = outH;
		queue.emplace_back(timestamp, std::move(frame));
	}

	if (first) {
		fail();
	}
	if (ownCom) {
		CoUninitialize();
	}
}

bool Playback::CreateTexture(ID3D11Device* device, const unsigned char* pixels, int frameWidth, int frameHeight, bool immutable)
{
	if (!device || frameWidth <= 0 || frameHeight <= 0) {
		return false;
	}
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = static_cast<UINT>(frameWidth);
	desc.Height = static_cast<UINT>(frameHeight);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	const bool dynamic = !immutable && state && state->dynamicUpload.load();
	desc.Usage = immutable ? D3D11_USAGE_IMMUTABLE : (dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT);
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
	D3D11_SUBRESOURCE_DATA data{};
	data.pSysMem = pixels;
	data.SysMemPitch = static_cast<UINT>(frameWidth) * 4;
	texture.Reset();
	view.Reset();
	textureDynamic = dynamic;
	if (FAILED(device->CreateTexture2D(&desc, (pixels && !dynamic) ? &data : nullptr, texture.GetAddressOf()))) {
		return false;
	}
	if (FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, view.GetAddressOf()))) {
		texture.Reset();
		return false;
	}
	return true;
}

void Playback::StopWorker()
{
	if (!state) {
		return;
	}
	state->run = false;
	state->ready.notify_all();
	ComPtr<IMFSourceReader> pending;
	{
		std::lock_guard lock(state->readerMutex);
		pending = state->reader;
	}
	if (pending) {
		pending->Flush(MF_SOURCE_READER_ALL_STREAMS);
	}
	ComPtr<IMFSourceReader> pendingAudio;
	{
		std::lock_guard lock(state->audioReaderMutex);
		pendingAudio = state->audioReader;
	}
	if (pendingAudio) {
		pendingAudio->Flush(MF_SOURCE_READER_ALL_STREAMS);
	}
	if (state->worker.joinable()) {
		state->worker.join();
	}
	if (state->audioWorker.joinable()) {
		state->audioWorker.join();
	}
}

void Playback::ReleaseAudio()
{
	if (!state) {
		return;
	}
	state->audible = false;
	if (state->voice) {
		state->voice->Stop();
		state->voice->FlushSourceBuffers();
		state->voice->DestroyVoice();
		state->voice = nullptr;
	}
	if (state->master) {
		state->master->DestroyVoice();
		state->master = nullptr;
	}
	state->audio.Reset();
}

void Playback::ApplyVolume()
{
	if (state && state->voice) {
		state->voice->SetVolume(state->volume);
	}
}
