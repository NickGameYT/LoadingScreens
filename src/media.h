#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

class Playback
{
public:
	Playback();
	~Playback();
	Playback(const Playback&) = delete;
	Playback& operator=(const Playback&) = delete;

	bool Open(ID3D11Device* device, const std::filesystem::path& path, float volume, bool playClipAudio, bool loop, const std::filesystem::path& externalAudio = {});
	void Close();
	void SetVolume(float volume);
	void SetFrameTarget(int width, int height);
	bool Update(ID3D11Device* device, ID3D11DeviceContext* context, int screenWidth, int screenHeight);
	bool TakeFinished();
	bool HasAudio() const;
	ID3D11ShaderResourceView* Texture() const;
	int Width() const;
	int Height() const;
	int Orientation() const;

	static bool IsMediaFile(const std::filesystem::path& path);
	static bool IsAudioFile(const std::filesystem::path& path);
	static bool IsEmergencyName(const std::filesystem::path& path);

private:
	struct SharedFrame;

	struct GpuDevice;
	struct GpuFrame;

	bool LoadStill(ID3D11Device* device, const std::filesystem::path& path);
	bool StartVideo(ID3D11Device* device, const std::filesystem::path& path);
	bool StartWebp(ID3D11Device* device, const std::filesystem::path& path);
	bool StartGif(const std::filesystem::path& path);
	void VideoMain(std::filesystem::path path);
	bool RunHardware(const std::filesystem::path& path);
	void RunSoftware(std::filesystem::path path);
	bool EnsureVideoDevice(ID3D11Device* gameDevice);
	bool EnsureGpuOutput(ID3D11Device* gameDevice, int sourceWidth, int sourceHeight, int outputWidth, int outputHeight, bool hd);
	void ReleaseGpuOutput();
	bool BlitGpu(ID3D11Device* gameDevice, ID3D11DeviceContext* gameContext, int screenWidth, int screenHeight, const std::shared_ptr<GpuFrame>& frame);
	std::vector<unsigned char> TakeSpare(std::size_t bytes);
	void Recycle(std::vector<unsigned char> buffer);
	void AudioMain(std::filesystem::path path);
	void BeginAudio(const std::filesystem::path& mediaPath);
	void WebpMain();
	void GifMain(std::filesystem::path path);
	void Publish(std::vector<unsigned char> rgba, int width, int height, bool first);
	bool CreateTexture(ID3D11Device* device, const unsigned char* pixels, int width, int height, bool immutable);
	void StopWorker();
	void ReleaseAudio();
	void ApplyVolume();

	Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
	int width = 0;
	int height = 0;
	int fitWidth = 0;
	int fitHeight = 0;
	int orientation = 0;
	int pendingW = 0;
	int pendingH = 0;
	bool textureDynamic = false;
	std::uint64_t shown = 0;

	struct State;
	std::unique_ptr<State> state;
	std::unique_ptr<GpuDevice> gpu;
};
