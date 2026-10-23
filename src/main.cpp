#include <SKSE/SKSE.h>
#include <RE/Skyrim.h>

#include <spdlog/sinks/basic_file_sink.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "media.h"

using Microsoft::WRL::ComPtr;

void ReadScreen(ID3D11DeviceContext* context, UINT& width, UINT& height)
{
	width = 0;
	height = 0;
	if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton()) {
		auto& window = renderer->data.renderWindows[0];
		width = static_cast<UINT>(window.windowWidth);
		height = static_cast<UINT>(window.windowHeight);
	}
	if (!context) {
		return;
	}
	UINT count = 1;
	D3D11_VIEWPORT viewport{};
	context->RSGetViewports(&count, &viewport);
	if (viewport.Width >= 1.0f && viewport.Height >= 1.0f) {
		width = static_cast<UINT>(viewport.Width);
		height = static_cast<UINT>(viewport.Height);
	}
}

namespace
{
	struct Settings
	{
		bool enabled = true;
		bool random = true;
		int randomMode = 2;
		bool playAudio = true;
		bool loadingFolderAudio = false;
		std::string loadingAudioFolder = "Audio";
		bool emergencyLoop = true;
		int emergencyKey = 0x23;
		int nextKey = 0x09;
		int volumeUpKey = 0x21;
		int volumeDownKey = 0x22;
		int fitMode = 0;
		float volume = 0.20f;
		float volumeStep = 0.10f;
		bool mainMenuVideo = false;
		bool mainMenuShared = true;
		bool mainMenuAudio = true;
		int mainMenuPlayback = 2;
		std::string mainMenuFolder = "Data\\MainMenuVideo";
	};

	class StateSaver
	{
	public:
		explicit StateSaver(ID3D11DeviceContext* context) :
			ctx(context)
		{
			ctx->OMGetRenderTargets(1, &rtv, &dsv);
			viewportCount = 1;
			ctx->RSGetViewports(&viewportCount, &viewport);
			ctx->OMGetBlendState(&blend, blendFactor, &blendMask);
			ctx->OMGetDepthStencilState(&depth, &stencilRef);
			ctx->RSGetState(&raster);
			ctx->VSGetShader(&vs, nullptr, nullptr);
			ctx->PSGetShader(&ps, nullptr, nullptr);
			ctx->IAGetPrimitiveTopology(&topology);
			ctx->IAGetInputLayout(&layout);
			ctx->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
			ctx->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
			ctx->VSGetConstantBuffers(0, 1, &vsCb);
			ctx->PSGetConstantBuffers(0, 1, &psCb);
			ctx->PSGetShaderResources(0, 1, &psSrv);
			ctx->PSGetSamplers(0, 1, &psSampler);
		}

		~StateSaver()
		{
			ctx->OMSetRenderTargets(1, &rtv, dsv);
			ctx->RSSetViewports(viewportCount, &viewport);
			ctx->OMSetBlendState(blend, blendFactor, blendMask);
			ctx->OMSetDepthStencilState(depth, stencilRef);
			ctx->RSSetState(raster);
			ctx->VSSetShader(vs, nullptr, 0);
			ctx->PSSetShader(ps, nullptr, 0);
			ctx->IASetPrimitiveTopology(topology);
			ctx->IASetInputLayout(layout);
			ctx->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
			ctx->IASetIndexBuffer(ib, ibFormat, ibOffset);
			ctx->VSSetConstantBuffers(0, 1, &vsCb);
			ctx->PSSetConstantBuffers(0, 1, &psCb);
			ctx->PSSetShaderResources(0, 1, &psSrv);
			ctx->PSSetSamplers(0, 1, &psSampler);

			Release(rtv);
			Release(dsv);
			Release(blend);
			Release(depth);
			Release(raster);
			Release(vs);
			Release(ps);
			Release(layout);
			Release(vb);
			Release(ib);
			Release(vsCb);
			Release(psCb);
			Release(psSrv);
			Release(psSampler);
		}

	private:
		template <class T>
		static void Release(T*& pointer)
		{
			if (pointer) {
				pointer->Release();
				pointer = nullptr;
			}
		}

		ID3D11DeviceContext* ctx = nullptr;
		ID3D11RenderTargetView* rtv = nullptr;
		ID3D11DepthStencilView* dsv = nullptr;
		UINT viewportCount = 0;
		D3D11_VIEWPORT viewport{};
		ID3D11BlendState* blend = nullptr;
		FLOAT blendFactor[4]{};
		UINT blendMask = 0;
		ID3D11DepthStencilState* depth = nullptr;
		UINT stencilRef = 0;
		ID3D11RasterizerState* raster = nullptr;
		ID3D11VertexShader* vs = nullptr;
		ID3D11PixelShader* ps = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ID3D11InputLayout* layout = nullptr;
		ID3D11Buffer* vb = nullptr;
		UINT vbStride = 0;
		UINT vbOffset = 0;
		ID3D11Buffer* ib = nullptr;
		DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
		UINT ibOffset = 0;
		ID3D11Buffer* vsCb = nullptr;
		ID3D11Buffer* psCb = nullptr;
		ID3D11ShaderResourceView* psSrv = nullptr;
		ID3D11SamplerState* psSampler = nullptr;
	};

	class Overlay : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static Overlay& Get()
		{
			static Overlay overlay;
			return overlay;
		}

		void Start()
		{
			if (started) {
				return;
			}
			started = true;
			InstallHook();
			if (auto* ui = RE::UI::GetSingleton()) {
				ui->AddEventSink(this);
				listening = true;
			}
			std::error_code directoryError;
			LoadSettings();
			foreignMenuVideo = GetModuleHandleA("po3_MainMenuVideo.dll") != nullptr;
			std::filesystem::create_directories(MediaDir(), directoryError);
			std::filesystem::create_directories(EmergencyDir(), directoryError);
			std::filesystem::create_directories(AudioDir(), directoryError);
			if (settings.mainMenuVideo && !settings.mainMenuShared && !foreignMenuVideo) {
				std::filesystem::create_directories(MenuFolder(), directoryError);
			}
			RefreshLibrary(mainLibrary, MediaDir(), true);
			RefreshLibrary(emergencyLibrary, EmergencyDir(), false);
			RefreshAudioLibrary();
			RefreshMenuLibrary();
			std::size_t mediaCount = 0;
			std::size_t emergencyCount = 0;
			std::size_t audioCount = 0;
			{
				std::lock_guard lock(libraryMutex);
				mediaCount = mainLibrary.files.size();
				emergencyCount = emergencyLibrary.files.size();
				audioCount = audioLibrary.files.size();
			}
			SKSE::log::info("Папка экранов: {}", MediaDir().string());
			SKSE::log::info("Экстренная папка: {}", EmergencyDir().string());
			SKSE::log::info("Папка звуков загрузки: {}", AudioDir().string());
			const auto menuFolder = settings.mainMenuShared ? MediaDir() : MenuFolder();
			SKSE::log::info("Главное меню: {}, папка: {}", settings.mainMenuVideo ? "включено" : "выключено", menuFolder.string());
			if (foreignMenuVideo) {
				SKSE::log::info("Найден Main Menu Video, свой фон меню не рисуется");
			}
			SKSE::log::info("Файлов: {}, экстренных: {}, звуков: {}", mediaCount, emergencyCount, audioCount);
			SKSE::log::info("Звук ролика загрузки: {}, папка звуков: {}, звук меню: {}", settings.playAudio ? "да" : "нет", settings.loadingFolderAudio ? "да" : "нет", settings.mainMenuAudio ? "да" : "нет");
			InstallMusicHook();
			if (auto* tasks = SKSE::GetTaskInterface()) {
				tasks->AddTask([]() { Get().InstallHook(); });
			}
		}

		void InstallHook()
		{
			REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_LoadingMenu[0] };
			const auto slot = vtable.address() + sizeof(void*) * 6;
			if (!slot) {
				SKSE::log::error("Адрес меню загрузки не найден");
				return;
			}
			auto* current = *reinterpret_cast<PostDisplayFn*>(slot);
			if (current == &PostDisplayThunk) {
				return;
			}
			originalPostDisplay = reinterpret_cast<PostDisplayFn>(vtable.write_vfunc(6, reinterpret_cast<std::uintptr_t>(&PostDisplayThunk)));
			REL::Relocation<std::uintptr_t> mainTable{ RE::VTABLE_MainMenu[0] };
			originalMainPost = reinterpret_cast<MainPostFn>(mainTable.write_vfunc(6, reinterpret_cast<std::uintptr_t>(&MainPostThunk)));
			SKSE::log::info("Фон меню загрузки и главного меню перехвачен");
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (!event) {
				return RE::BSEventNotifyControl::kContinue;
			}
			if (event->menuName == RE::LoadingMenu::MENU_NAME && !event->opening && !menuSession) {
				playback.Close();
				wasLoading = false;
				emergencyLatched = false;
				toastUntil = {};
			} else if (event->menuName == RE::MainMenu::MENU_NAME && !event->opening) {
				seenGame = true;
				quittingToMenu = false;
				menuSession = false;
				playback.Close();
				wasLoading = false;
				emergencyLatched = false;
				toastUntil = {};
			} else if (event->menuName == RE::FaderMenu::MENU_NAME && event->opening) {
				if (auto* main = RE::Main::GetSingleton(); main && main->resetGame) {
					quittingToMenu = true;
					seenGame = false;
					wasLoading = false;
					menuSession = false;
					emergencyLatched = false;
					playback.Close();
					toastUntil = {};
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}

	private:
		struct Library;

		using PostDisplayFn = void (*)(RE::LoadingMenu*);

		static void PostDisplayThunk(RE::LoadingMenu* menu)
		{
			Get().OnLoadingFrame(menu);
			if (originalPostDisplay && originalPostDisplay != &PostDisplayThunk) {
				originalPostDisplay(menu);
			}
		}

		void OnLoadingFrame(RE::LoadingMenu* menu)
		{
			if (!loggedFrame) {
				loggedFrame = true;
				SKSE::log::info("Кадр меню загрузки");
			}
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			if (!renderer) {
				if (!loggedDevice) {
					loggedDevice = true;
					SKSE::log::warn("Отрисовщик игры ещё не готов");
				}
				return;
			}
			auto& window = renderer->data.renderWindows[0];
			auto* device = reinterpret_cast<ID3D11Device*>(renderer->data.forwarder);
			auto* context = reinterpret_cast<ID3D11DeviceContext*>(renderer->data.context);
			ComPtr<ID3D11Device> ownedDevice;
			if (!device && window.swapChain) {
				reinterpret_cast<IDXGISwapChain*>(window.swapChain)->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(ownedDevice.GetAddressOf()));
				device = ownedDevice.Get();
			}
			ComPtr<ID3D11DeviceContext> ownedContext;
			if (device && !context) {
				device->GetImmediateContext(ownedContext.GetAddressOf());
				context = ownedContext.Get();
			}
			if (!device || !context) {
				if (!loggedDevice) {
					loggedDevice = true;
					SKSE::log::warn("Нет устройства отрисовки");
				}
				return;
			}
			UINT screenW = 0;
			UINT screenH = 0;
			ReadScreen(context, screenW, screenH);
			playback.SetFrameTarget(static_cast<int>(screenW), static_cast<int>(screenH));
			if (device != boundDevice) {
				pipeline.Reset();
				playback.Close();
				boundDevice = device;
				wasLoading = false;
				menuSession = false;
				toastDirty = true;
			}
			if (!listening) {
				if (auto* ui = RE::UI::GetSingleton()) {
					ui->AddEventSink(this);
					listening = true;
				}
			}
			if (MenuContext()) {
				if (foreignMenuVideo || !settings.mainMenuVideo) {
					if (menuSession || wasLoading) {
						playback.Close();
						menuSession = false;
						wasLoading = false;
						emergencyLatched = false;
						toastUntil = {};
					}
					return;
				}
				ShowMenuFrame(device, context, menu, true);
				return;
			}
			if (menuSession) {
				playback.Close();
				menuSession = false;
			}
			if (!wasLoading) {
				LoadSettings();
				emergencyLatched = false;
				ResetKeys();
				playback.Close();
				RefreshLibrary(mainLibrary, MediaDir(), true);
				RefreshLibrary(emergencyLibrary, EmergencyDir(), false);
				if (settings.enabled) {
					Pick(true);
				}
			}
			wasLoading = true;
			if (!settings.enabled) {
				return;
			}

			HandleKeys(false);
			playback.Update(device, context, static_cast<int>(screenW), static_cast<int>(screenH));
			int advances = 0;
			while (emergencyLatched && !settings.emergencyLoop && playback.TakeFinished() && advances < 4) {
				++advances;
				PickEmergency(false, settings.playAudio);
			}
			if (!playback.Texture() || !EnsurePipeline(device)) {
				if (!loggedTexture) {
					loggedTexture = true;
					SKSE::log::warn("Кадр экрана не подготовлен");
				}
				return;
			}

			ComPtr<ID3D11RenderTargetView> currentTarget;
			context->OMGetRenderTargets(1, currentTarget.GetAddressOf(), nullptr);
			ID3D11RenderTargetView* target = currentTarget.Get();
			if (!target) {
				target = reinterpret_cast<ID3D11RenderTargetView*>(window.renderView);
			}
			screenW = static_cast<UINT>(window.windowWidth);
			screenH = static_cast<UINT>(window.windowHeight);
			UINT viewportCount = 1;
			D3D11_VIEWPORT viewport{};
			context->RSGetViewports(&viewportCount, &viewport);
			if (viewport.Width >= 1.0f && viewport.Height >= 1.0f) {
				screenW = static_cast<UINT>(viewport.Width);
				screenH = static_cast<UINT>(viewport.Height);
			}
			if (!target || screenW < 1 || screenH < 1) {
				if (!loggedTarget) {
					loggedTarget = true;
					SKSE::log::warn("Нет цели отрисовки {}x{}", screenW, screenH);
				}
				return;
			}
			HideVanillaArt(menu);
			StateSaver saved(context);
			Draw(context, target, screenW, screenH);
		}

		static void HideVanillaArt(RE::LoadingMenu* menu)
		{
			if (!menu) {
				return;
			}
			auto& root = menu->GetRuntimeData().root;
			if (!root.IsObject()) {
				return;
			}
			RE::GFxValue background;
			if (root.GetMember("Background", &background) && background.IsDisplayObject()) {
				background.SetMember("_visible", false);
			}
		}

		using MainPostFn = void (*)(RE::MainMenu*);
		using MusicFn = bool (*)();

		static void MainPostThunk(RE::MainMenu* menu)
		{
			Get().OnMainMenuFrame();
			if (originalMainPost && originalMainPost != &MainPostThunk) {
				originalMainPost(menu);
			}
		}

		static bool MusicThunkA()
		{
			if (Get().MenuOwnsMusic()) {
				return true;
			}
			return originalMusicA ? originalMusicA() : false;
		}

		static bool MusicThunkB()
		{
			if (Get().MenuOwnsMusic()) {
				return true;
			}
			return originalMusicB ? originalMusicB() : false;
		}

		bool MenuContext() const
		{
			if (!seenGame || quittingToMenu) {
				return true;
			}
			if (auto* main = RE::Main::GetSingleton(); main && main->resetGame) {
				return true;
			}
			if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {
				return true;
			}
			return false;
		}

		bool MenuOwnsMusic() const
		{
			return settings.mainMenuVideo && settings.mainMenuAudio && !foreignMenuVideo && MenuContext();
		}

		void OnMainMenuFrame()
		{
			if (foreignMenuVideo || !settings.mainMenuVideo) {
				return;
			}
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			if (!renderer) {
				return;
			}
			auto& window = renderer->data.renderWindows[0];
			auto* device = reinterpret_cast<ID3D11Device*>(renderer->data.forwarder);
			auto* context = reinterpret_cast<ID3D11DeviceContext*>(renderer->data.context);
			ComPtr<ID3D11Device> ownedDevice;
			if (!device && window.swapChain) {
				reinterpret_cast<IDXGISwapChain*>(window.swapChain)->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(ownedDevice.GetAddressOf()));
				device = ownedDevice.Get();
			}
			ComPtr<ID3D11DeviceContext> ownedContext;
			if (device && !context) {
				device->GetImmediateContext(ownedContext.GetAddressOf());
				context = ownedContext.Get();
			}
			if (!device || !context) {
				return;
			}
			if (device != boundDevice) {
				pipeline.Reset();
				playback.Close();
				boundDevice = device;
				menuSession = false;
				wasLoading = false;
				toastDirty = true;
			}
			ShowMenuFrame(device, context, nullptr, false);
		}

		void ShowMenuFrame(ID3D11Device* device, ID3D11DeviceContext* context, RE::LoadingMenu* menu, bool hideArt)
		{
			UINT screenW = 0;
			UINT screenH = 0;
			ReadScreen(context, screenW, screenH);
			playback.SetFrameTarget(static_cast<int>(screenW), static_cast<int>(screenH));
			if (!menuSession) {
				LoadSettings();
				ResetKeys();
				playback.Close();
				RefreshMenuLibrary();
				RefreshLibrary(emergencyLibrary, EmergencyDir(), false);
				const bool loop = settings.mainMenuPlayback == 2;
				OpenFrom(menuLibrary, menuIndex, true, loop, "Главное меню", false, settings.mainMenuAudio, false);
				menuSession = true;
			}
			HandleKeys(true);
			playback.Update(device, context, static_cast<int>(screenW), static_cast<int>(screenH));
			if (emergencyLatched) {
				int advances = 0;
				while (!settings.emergencyLoop && playback.TakeFinished() && advances < 4) {
					++advances;
					PickEmergency(false, settings.mainMenuAudio);
				}
			} else if (settings.mainMenuPlayback == 1 && playback.TakeFinished()) {
				OpenFrom(menuLibrary, menuIndex, false, false, "Главное меню", true, settings.mainMenuAudio, false);
			} else if (settings.mainMenuPlayback == 0) {
				playback.TakeFinished();
			}
			if (!playback.Texture() || !EnsurePipeline(device)) {
				return;
			}
			Paint(context, menu, hideArt);
		}

		void Paint(ID3D11DeviceContext* context, RE::LoadingMenu* menu, bool hideArt)
		{
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			if (!renderer) {
				return;
			}
			auto& window = renderer->data.renderWindows[0];
			ComPtr<ID3D11RenderTargetView> currentTarget;
			context->OMGetRenderTargets(1, currentTarget.GetAddressOf(), nullptr);
			ID3D11RenderTargetView* target = currentTarget.Get();
			if (!target) {
				target = reinterpret_cast<ID3D11RenderTargetView*>(window.renderView);
			}
			UINT screenW = static_cast<UINT>(window.windowWidth);
			UINT screenH = static_cast<UINT>(window.windowHeight);
			UINT viewportCount = 1;
			D3D11_VIEWPORT viewport{};
			context->RSGetViewports(&viewportCount, &viewport);
			if (viewport.Width >= 1.0f && viewport.Height >= 1.0f) {
				screenW = static_cast<UINT>(viewport.Width);
				screenH = static_cast<UINT>(viewport.Height);
			}
			if (!target || screenW < 1 || screenH < 1) {
				return;
			}
			if (hideArt) {
				HideVanillaArt(menu);
			}
			StateSaver saved(context);
			Draw(context, target, screenW, screenH);
		}

		void RefreshMenuLibrary()
		{
			const auto primary = settings.mainMenuShared ? MediaDir() : MenuFolder();
			RefreshLibrary(menuLibrary, primary, true);
			bool empty = false;
			{
				std::lock_guard lock(libraryMutex);
				empty = menuLibrary.files.empty();
			}
			const auto fallback = std::filesystem::path("Data") / "MainMenuVideo";
			std::error_code sameError;
			const bool same = std::filesystem::equivalent(primary, fallback, sameError);
			std::error_code existsError;
			if (empty && (sameError || !same) && std::filesystem::exists(fallback, existsError)) {
				RefreshLibrary(menuLibrary, fallback, true);
			}
		}

		std::filesystem::path MenuFolder() const
		{
			std::filesystem::path folder = settings.mainMenuFolder.empty() ? std::filesystem::path("Data") / "MainMenuVideo" : std::filesystem::path(settings.mainMenuFolder);
			if (folder.is_absolute()) {
				return folder;
			}
			const auto first = folder.begin();
			if (first != folder.end() && _wcsicmp(first->c_str(), L"Data") == 0) {
				return folder;
			}
			return DllDir() / folder;
		}

		void InstallMusicHook()
		{
			if (musicHooked || foreignMenuVideo) {
				return;
			}
			const auto runtime = REL::Module::GetRuntime();
			if (runtime == REL::Module::Runtime::VR) {
				return;
			}
			const std::ptrdiff_t firstOffset = runtime == REL::Module::Runtime::AE ? 0x2A : 0x19;
			const std::ptrdiff_t secondOffset = runtime == REL::Module::Runtime::AE ? 0x2DD : 0xFD;
			auto& trampoline = SKSE::GetTrampoline();
			REL::Relocation<std::uintptr_t> first{ REL::RelocationID(51238, 52110), firstOffset };
			REL::Relocation<std::uintptr_t> second{ REL::RelocationID(51259, 52137), secondOffset };
			if (!first.address() || !second.address()) {
				SKSE::log::warn("Музыка главного меню не перехвачена");
				return;
			}
			originalMusicA = reinterpret_cast<MusicFn>(trampoline.write_call<5>(first.address(), &MusicThunkA));
			originalMusicB = reinterpret_cast<MusicFn>(trampoline.write_call<5>(second.address(), &MusicThunkB));
			musicHooked = true;
			SKSE::log::info("Музыка главного меню уступает звуку ролика");
		}

		void HandleKeys(bool menuMode)
		{
			auto* ui = RE::UI::GetSingleton();
			if (ui && ui->IsMenuOpen("Console")) {
				return;
			}
			if (Pressed(settings.emergencyKey, emergencyDown)) {
				pendingAction = 2;
			}
			if (Pressed(settings.nextKey, nextDown)) {
				pendingAction = 1;
			}
			const auto now = std::chrono::steady_clock::now();
			if (pendingAction != 0 && !switching && now >= switchReady) {
				switching = true;
				const int action = pendingAction;
				pendingAction = 0;
				switchReady = now + std::chrono::milliseconds(200);
				try {
					if (action == 2) {
						if (emergencyLatched) {
							emergencyLatched = false;
							if (menuMode) {
								const bool loop = settings.mainMenuPlayback == 2;
								OpenFrom(menuLibrary, menuIndex, false, loop, "Главное меню", false, settings.mainMenuAudio, false);
							} else {
								OpenFrom(mainLibrary, mainIndex, false, true, "Экран загрузки", false, settings.playAudio, true);
							}
						} else {
							emergencyLatched = true;
							PickEmergency(true, menuMode ? settings.mainMenuAudio : settings.playAudio);
						}
					} else if (menuMode && emergencyLatched) {
						PickEmergency(false, settings.mainMenuAudio);
					} else if (menuMode) {
						const bool loop = settings.mainMenuPlayback == 2;
						OpenFrom(menuLibrary, menuIndex, false, loop, "Главное меню", true, settings.mainMenuAudio, false);
					} else if (emergencyLatched) {
						PickEmergency(false, settings.playAudio);
					} else {
						Pick(false);
					}
				} catch (...) {
					SKSE::log::error("Переключение ролика прервалось");
				}
				switching = false;
			}
			if (Pressed(settings.volumeUpKey, upDown)) {
				ChangeVolume(settings.volumeStep);
			}
			if (Pressed(settings.volumeDownKey, downDown)) {
				ChangeVolume(-settings.volumeStep);
			}
		}

		void ChangeVolume(float step)
		{
			settings.volume = std::clamp(settings.volume + step, 0.0f, 1.0f);
			playback.SetVolume(settings.volume);
			WriteVolume();
			wchar_t text[16]{};
			std::swprintf(text, 16, L"%d%%", static_cast<int>(std::lround(settings.volume * 100.0f)));
			toastText = text;
			toastDirty = true;
			toastUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(1600);
			SKSE::log::info("Громкость экрана загрузки: {}%", static_cast<int>(settings.volume * 100.0f + 0.5f));
		}

		void Draw(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, UINT screenW, UINT screenH)
		{
			const int imageW = playback.Width();
			const int imageH = playback.Height();
			if (screenW == 0 || screenH == 0 || imageW <= 0 || imageH <= 0) {
				return;
			}

			float offsetX = 0.0f;
			float offsetY = 0.0f;
			float scaleX = 1.0f;
			float scaleY = 1.0f;
			if (settings.fitMode != 1) {
				const float sx = static_cast<float>(screenW) / static_cast<float>(imageW);
				const float sy = static_cast<float>(screenH) / static_cast<float>(imageH);
				const float scale = settings.fitMode == 2 ? (std::max)(sx, sy) : (std::min)(sx, sy);
				const float drawW = static_cast<float>(imageW) * scale;
				const float drawH = static_cast<float>(imageH) * scale;
				scaleX = drawW / static_cast<float>(screenW);
				scaleY = drawH / static_cast<float>(screenH);
				offsetX = (1.0f - scaleX) * 0.5f;
				offsetY = (1.0f - scaleY) * 0.5f;
			}
			const float fit[8] = {
				offsetX,
				offsetY,
				(std::max)(scaleX, 0.0001f),
				(std::max)(scaleY, 0.0001f),
				static_cast<float>(playback.Orientation()),
				0.0f,
				0.0f,
				0.0f
			};
			context->UpdateSubresource(pipeline.constants.Get(), 0, nullptr, fit, 0, 0);

			D3D11_VIEWPORT viewport{};
			viewport.Width = static_cast<float>(screenW);
			viewport.Height = static_cast<float>(screenH);
			viewport.MaxDepth = 1.0f;

			context->OMSetRenderTargets(1, &target, nullptr);
			context->RSSetViewports(1, &viewport);
			context->RSSetState(pipeline.raster.Get());
			context->OMSetBlendState(pipeline.blend.Get(), nullptr, 0xFFFFFFFF);
			context->OMSetDepthStencilState(pipeline.depth.Get(), 0);
			context->IASetInputLayout(nullptr);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->VSSetShader(pipeline.vs.Get(), nullptr, 0);
			context->PSSetShader(pipeline.ps.Get(), nullptr, 0);
			ID3D11Buffer* constants = pipeline.constants.Get();
			context->VSSetConstantBuffers(0, 1, &constants);
			context->PSSetConstantBuffers(0, 1, &constants);
			ID3D11ShaderResourceView* srv = playback.Texture();
			context->PSSetShaderResources(0, 1, &srv);
			ID3D11SamplerState* sampler = pipeline.sampler.Get();
			context->PSSetSamplers(0, 1, &sampler);
			context->Draw(3, 0);
			DrawToast(context, screenW, screenH);
		}

		void DrawToast(ID3D11DeviceContext* context, UINT screenW, UINT screenH)
		{
			if (toastText.empty() || std::chrono::steady_clock::now() >= toastUntil) {
				return;
			}
			if (!pipeline.spriteVs || !boundDevice) {
				return;
			}
			if (toastDirty && !BuildToast(boundDevice)) {
				return;
			}
			if (!pipeline.toastView || toastW <= 0 || toastH <= 0) {
				return;
			}
			const float block[8] = {
				0.0f,
				0.0f,
				static_cast<float>(toastW),
				static_cast<float>(toastH),
				static_cast<float>(screenW),
				static_cast<float>(screenH),
				0.0f,
				0.0f
			};
			context->UpdateSubresource(pipeline.spriteConstants.Get(), 0, nullptr, block, 0, 0);
			context->OMSetBlendState(pipeline.spriteBlend.Get(), nullptr, 0xFFFFFFFF);
			context->VSSetShader(pipeline.spriteVs.Get(), nullptr, 0);
			context->PSSetShader(pipeline.spritePs.Get(), nullptr, 0);
			ID3D11Buffer* constants = pipeline.spriteConstants.Get();
			context->VSSetConstantBuffers(0, 1, &constants);
			ID3D11ShaderResourceView* srv = pipeline.toastView.Get();
			context->PSSetShaderResources(0, 1, &srv);
			ID3D11SamplerState* sampler = pipeline.sampler.Get();
			context->PSSetSamplers(0, 1, &sampler);
			context->Draw(3, 0);
		}

		bool BuildToast(ID3D11Device* device)
		{
			toastDirty = false;
			pipeline.toastView.Reset();
			pipeline.toastTexture.Reset();
			HDC screen = GetDC(nullptr);
			HDC dc = CreateCompatibleDC(screen);
			HFONT font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
			HGDIOBJ oldFont = SelectObject(dc, font);
			SIZE size{};
			GetTextExtentPoint32W(dc, toastText.c_str(), static_cast<int>(toastText.size()), &size);
			toastW = std::max(24, static_cast<int>(size.cx) + 12);
			toastH = std::max(16, static_cast<int>(size.cy) + 6);
			BITMAPINFO info{};
			info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			info.bmiHeader.biWidth = toastW;
			info.bmiHeader.biHeight = -toastH;
			info.bmiHeader.biPlanes = 1;
			info.bmiHeader.biBitCount = 32;
			info.bmiHeader.biCompression = BI_RGB;
			void* bits = nullptr;
			HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
			HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
			RECT plate{ 0, 0, toastW, toastH };
			HBRUSH brush = CreateSolidBrush(RGB(12, 12, 12));
			FillRect(dc, &plate, brush);
			DeleteObject(brush);
			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, RGB(255, 255, 255));
			DrawTextW(dc, toastText.c_str(), -1, &plate, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			std::vector<unsigned char> rgba(static_cast<std::size_t>(toastW) * toastH * 4);
			const auto* source = static_cast<const unsigned char*>(bits);
			for (int i = 0; i < toastW * toastH; ++i) {
				rgba[static_cast<std::size_t>(i) * 4 + 0] = source[static_cast<std::size_t>(i) * 4 + 2];
				rgba[static_cast<std::size_t>(i) * 4 + 1] = source[static_cast<std::size_t>(i) * 4 + 1];
				rgba[static_cast<std::size_t>(i) * 4 + 2] = source[static_cast<std::size_t>(i) * 4 + 0];
				rgba[static_cast<std::size_t>(i) * 4 + 3] = 255;
			}
			SelectObject(dc, oldBitmap);
			SelectObject(dc, oldFont);
			DeleteObject(bitmap);
			DeleteObject(font);
			DeleteDC(dc);
			ReleaseDC(nullptr, screen);

			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = static_cast<UINT>(toastW);
			desc.Height = static_cast<UINT>(toastH);
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA data{};
			data.pSysMem = rgba.data();
			data.SysMemPitch = static_cast<UINT>(toastW) * 4;
			if (FAILED(device->CreateTexture2D(&desc, &data, pipeline.toastTexture.GetAddressOf()))) {
				return false;
			}
			if (FAILED(device->CreateShaderResourceView(pipeline.toastTexture.Get(), nullptr, pipeline.toastView.GetAddressOf()))) {
				pipeline.toastTexture.Reset();
				return false;
			}
			return true;
		}

		bool EnsurePipeline(ID3D11Device* device)
		{
			if (pipeline.vs && pipeline.ps) {
				return true;
			}

			const char* shader = R"HLSL(
cbuffer Fit : register(b0) { float4 fit; float4 orient; };
Texture2D image : register(t0);
SamplerState imageSampler : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut output;
    output.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}
float4 PSMain(VSOut input) : SV_Target {
    float2 coord = (input.uv - fit.xy) / fit.zw;
    if (coord.x < 0.0 || coord.y < 0.0 || coord.x > 1.0 || coord.y > 1.0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    float2 centered = coord - 0.5;
    if (orient.x > 2.5) {
        centered = float2(-centered.y, centered.x);
    } else if (orient.x > 1.5) {
        centered = -centered;
    } else if (orient.x > 0.5) {
        centered = float2(centered.y, -centered.x);
    }
    coord = centered + 0.5;
    float4 color = image.Sample(imageSampler, coord);
    return float4(color.rgb, 1.0);
}
)HLSL";

			ComPtr<ID3DBlob> vsBlob;
			ComPtr<ID3DBlob> psBlob;
			ComPtr<ID3DBlob> errors;
			if (FAILED(D3DCompile(shader, std::strlen(shader), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, vsBlob.GetAddressOf(), errors.GetAddressOf()))) {
				LogBlob(errors.Get());
				return false;
			}
			errors.Reset();
			if (FAILED(D3DCompile(shader, std::strlen(shader), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, psBlob.GetAddressOf(), errors.GetAddressOf()))) {
				LogBlob(errors.Get());
				return false;
			}
			if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, pipeline.vs.GetAddressOf()))) {
				return false;
			}
			if (FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, pipeline.ps.GetAddressOf()))) {
				return false;
			}

			D3D11_BUFFER_DESC cb{};
			cb.ByteWidth = 32;
			cb.Usage = D3D11_USAGE_DEFAULT;
			cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			if (FAILED(device->CreateBuffer(&cb, nullptr, pipeline.constants.GetAddressOf()))) {
				return false;
			}

			D3D11_SAMPLER_DESC sampler{};
			sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampler.MaxLOD = D3D11_FLOAT32_MAX;
			if (FAILED(device->CreateSamplerState(&sampler, pipeline.sampler.GetAddressOf()))) {
				return false;
			}

			D3D11_BLEND_DESC blend{};
			blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			if (FAILED(device->CreateBlendState(&blend, pipeline.blend.GetAddressOf()))) {
				return false;
			}

			D3D11_DEPTH_STENCIL_DESC depth{};
			depth.DepthEnable = FALSE;
			depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			if (FAILED(device->CreateDepthStencilState(&depth, pipeline.depth.GetAddressOf()))) {
				return false;
			}

			D3D11_RASTERIZER_DESC raster{};
			raster.FillMode = D3D11_FILL_SOLID;
			raster.CullMode = D3D11_CULL_NONE;
			raster.DepthClipEnable = TRUE;
			if (FAILED(device->CreateRasterizerState(&raster, pipeline.raster.GetAddressOf()))) {
				return false;
			}

			const char* spriteShader = R"HLSL(
cbuffer Sprite : register(b0) { float4 rect; float4 screen; };
Texture2D sprite : register(t0);
SamplerState spriteSampler : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut SpriteVS(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    float2 pixel = rect.xy + uv * rect.zw;
    VSOut output;
    output.pos = float4(pixel.x / screen.x * 2.0 - 1.0, 1.0 - pixel.y / screen.y * 2.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}
float4 SpritePS(VSOut input) : SV_Target {
    if (input.uv.x < 0.0 || input.uv.y < 0.0 || input.uv.x > 1.0 || input.uv.y > 1.0) {
        discard;
    }
    return sprite.Sample(spriteSampler, input.uv);
}
)HLSL";
			vsBlob.Reset();
			psBlob.Reset();
			errors.Reset();
			if (FAILED(D3DCompile(spriteShader, std::strlen(spriteShader), nullptr, nullptr, nullptr, "SpriteVS", "vs_5_0", 0, 0, vsBlob.GetAddressOf(), errors.GetAddressOf())) ||
				FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, pipeline.spriteVs.GetAddressOf()))) {
				LogBlob(errors.Get());
				return true;
			}
			errors.Reset();
			if (FAILED(D3DCompile(spriteShader, std::strlen(spriteShader), nullptr, nullptr, nullptr, "SpritePS", "ps_5_0", 0, 0, psBlob.GetAddressOf(), errors.GetAddressOf())) ||
				FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, pipeline.spritePs.GetAddressOf()))) {
				LogBlob(errors.Get());
				return true;
			}
			D3D11_BUFFER_DESC spriteCb{};
			spriteCb.ByteWidth = 32;
			spriteCb.Usage = D3D11_USAGE_DEFAULT;
			spriteCb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			device->CreateBuffer(&spriteCb, nullptr, pipeline.spriteConstants.GetAddressOf());
			D3D11_BLEND_DESC spriteBlend{};
			spriteBlend.RenderTarget[0].BlendEnable = TRUE;
			spriteBlend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			spriteBlend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			spriteBlend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			spriteBlend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			spriteBlend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			spriteBlend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			spriteBlend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			device->CreateBlendState(&spriteBlend, pipeline.spriteBlend.GetAddressOf());
			return true;
		}

		void Pick(bool randomStart)
		{
			OpenFrom(mainLibrary, mainIndex, randomStart, true, "Экран загрузки", true, settings.playAudio, true);
		}

		void PickEmergency(bool randomStart, bool audio)
		{
			bool empty = false;
			{
				std::lock_guard lock(libraryMutex);
				empty = emergencyLibrary.files.empty();
			}
			if (empty) {
				SKSE::log::warn("В экстренной папке нет файлов: {}", EmergencyDir().string());
				return;
			}
			OpenFrom(emergencyLibrary, emergencyIndex, randomStart, settings.emergencyLoop, "Экстренный экран", true, audio, true);
		}

		std::filesystem::path TakeLoadingSound()
		{
			RefreshAudioLibrary();
			std::lock_guard lock(libraryMutex);
			if (audioLibrary.files.empty()) {
				return {};
			}
			const auto& files = audioLibrary.files;
			if (audioIndex >= files.size()) {
				audioIndex = NextIndex(audioLibrary, audioIndex, true, false, files);
			} else {
				audioIndex = NextIndex(audioLibrary, audioIndex, false, true, files);
			}
			return files[audioIndex];
		}

		void RebuildDeck(Library& library, const std::vector<std::filesystem::path>& files, std::size_t avoid)
		{
			library.deck.resize(files.size());
			std::iota(library.deck.begin(), library.deck.end(), std::size_t{ 0 });
			std::shuffle(library.deck.begin(), library.deck.end(), rng);
			if (files.size() > 1 && library.deck.front() == avoid) {
				std::swap(library.deck.front(), library.deck.back());
			}
			library.deckPos = 0;
		}

		std::size_t NextIndex(Library& library, std::size_t& cursor, bool randomStart, bool advance, const std::vector<std::filesystem::path>& files)
		{
			const std::size_t count = files.size();
			if (count == 0) {
				return 0;
			}
			if (cursor >= count) {
				cursor = 0;
			}
			const int mode = settings.randomMode;
			if (mode <= 0) {
				if (randomStart) {
					return 0;
				}
				if (advance) {
					return (cursor + 1) % count;
				}
				return cursor;
			}
			if (mode == 1) {
				if (!randomStart && !advance) {
					return cursor;
				}
				std::uniform_int_distribution<std::size_t> dist(0, count - 1);
				std::size_t next = dist(rng);
				if (count > 1) {
					for (int tries = 0; tries < 8 && next == cursor; ++tries) {
						next = dist(rng);
					}
				}
				return next;
			}
			if (library.deck.size() != count || library.deckPos > library.deck.size()) {
				RebuildDeck(library, files, cursor);
			}
			if (randomStart || advance) {
				if (library.deckPos >= library.deck.size()) {
					RebuildDeck(library, files, cursor);
				}
				return library.deck[library.deckPos++];
			}
			return cursor;
		}

		bool OpenFrom(Library& library, std::size_t& cursor, bool randomStart, bool loop, const char* label, bool advance, bool clipAudio, bool folderAudio)
		{
			std::vector<std::filesystem::path> files;
			{
				std::lock_guard lock(libraryMutex);
				files = library.files;
			}
			if (files.empty()) {
				playback.Close();
				if (!emergencyLatched) {
					SKSE::log::warn("В папке нет картинок и видео: {}", MediaDir().string());
				}
				return false;
			}
			{
				std::lock_guard lock(libraryMutex);
				cursor = NextIndex(library, cursor, randomStart, advance, files);
			}

			std::filesystem::path separateAudio;
			if (!clipAudio && folderAudio && settings.loadingFolderAudio) {
				separateAudio = TakeLoadingSound();
				if (separateAudio.empty()) {
					SKSE::log::warn("Папка звуков загрузки пуста: {}", AudioDir().string());
				}
			}

			const std::size_t start = cursor;
			int attempts = 0;
			do {
				const auto path = files[cursor];
				if (playback.Open(boundDevice, path, settings.volume, clipAudio, loop, separateAudio)) {
					if (!separateAudio.empty()) {
						SKSE::log::info("{}: {} + {}", label, path.filename().string(), separateAudio.filename().string());
					} else {
						SKSE::log::info("{}: {}", label, path.filename().string());
					}
					return true;
				}
				if (++attempts >= 8) {
					break;
				}
				std::lock_guard lock(libraryMutex);
				if (settings.randomMode >= 2) {
					if (library.deckPos >= library.deck.size()) {
						RebuildDeck(library, files, cursor);
					}
					cursor = library.deckPos < library.deck.size() ? library.deck[library.deckPos++] : (cursor + 1) % files.size();
				} else if (settings.randomMode == 1) {
					std::uniform_int_distribution<std::size_t> dist(0, files.size() - 1);
					cursor = dist(rng);
				} else {
					cursor = (cursor + 1) % files.size();
				}
			} while (cursor != start);
			playback.Close();
			SKSE::log::warn("Файл экрана не открылся");
			return false;
		}

		void RefreshLibrary(Library& library, const std::filesystem::path& directory, bool skipEmergencyName)
		{
			std::error_code error;
			std::filesystem::create_directories(directory, error);
			const auto stamp = std::filesystem::last_write_time(directory, error);
			{
				std::lock_guard lock(libraryMutex);
				if (!error && library.ready && stamp == library.stamp) {
					return;
				}
			}
			std::vector<std::filesystem::path> files;
			WIN32_FIND_DATAW data{};
			const auto pattern = (directory / L"*").wstring();
			HANDLE handle = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
			if (handle != INVALID_HANDLE_VALUE) {
				do {
					if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
						continue;
					}
					std::filesystem::path path = directory / data.cFileName;
					if (!Playback::IsMediaFile(path)) {
						continue;
					}
					if (skipEmergencyName && Playback::IsEmergencyName(path)) {
						continue;
					}
					files.push_back(std::move(path));
				} while (FindNextFileW(handle, &data));
				FindClose(handle);
			}
			std::sort(files.begin(), files.end());
			std::lock_guard lock(libraryMutex);
			const bool changed = library.files != files;
			library.files = std::move(files);
			library.stamp = stamp;
			library.ready = !error;
			if (changed) {
				library.deck.clear();
				library.deckPos = 0;
			}
		}

		void LoadSettings()
		{
			const float keptVolume = settings.volume;
			const bool keepVolume = wasLoading;
			settings = {};
			bool hasRandomMode = false;
			std::ifstream file(IniPath());
			if (!file) {
				WriteDefaultIni();
				return;
			}
			std::string line;
			while (std::getline(file, line)) {
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				const auto comment = line.find_first_of(";#");
				if (comment != std::string::npos) {
					line.erase(comment);
				}
				const auto split = line.find('=');
				if (split == std::string::npos) {
					continue;
				}
				auto key = Trim(line.substr(0, split));
				auto value = Trim(line.substr(split + 1));
				if (key == "bEnabled") {
					settings.enabled = value != "false" && value != "0";
				} else if (key == "iRandomMode") {
					settings.randomMode = std::clamp(std::atoi(value.c_str()), 0, 2);
					hasRandomMode = true;
				} else if (key == "bRandom") {
					settings.random = value != "false" && value != "0";
				} else if (key == "bPlayAudio") {
					settings.playAudio = value != "false" && value != "0";
				} else if (key == "bLoadingFolderAudio") {
					settings.loadingFolderAudio = value != "false" && value != "0";
				} else if (key == "sLoadingAudioFolder") {
					settings.loadingAudioFolder = value;
				} else if (key == "bEmergencyLoop") {
					settings.emergencyLoop = value != "false" && value != "0";
				} else if (key == "iEmergencyKey") {
					settings.emergencyKey = std::atoi(value.c_str());
				} else if (key == "iPlayNextKey") {
					settings.nextKey = std::atoi(value.c_str());
				} else if (key == "iVolumeUpKey") {
					settings.volumeUpKey = std::atoi(value.c_str());
				} else if (key == "iVolumeDownKey") {
					settings.volumeDownKey = std::atoi(value.c_str());
				} else if (key == "iFitMode") {
					settings.fitMode = std::atoi(value.c_str());
				} else if (key == "fVolume") {
					settings.volume = ParseUnit(value);
				} else if (key == "fVolumeStep") {
					settings.volumeStep = ParseUnit(value);
				} else if (key == "bMainMenuVideo") {
					settings.mainMenuVideo = value != "false" && value != "0";
				} else if (key == "bMainMenuSharedFolder") {
					settings.mainMenuShared = value != "false" && value != "0";
				} else if (key == "bMainMenuAudio") {
					settings.mainMenuAudio = value != "false" && value != "0";
				} else if (key == "iMainMenuPlayback") {
					settings.mainMenuPlayback = std::clamp(std::atoi(value.c_str()), 0, 2);
				} else if (key == "sMainMenuFolder") {
					settings.mainMenuFolder = value;
				}
			}
			if (!hasRandomMode) {
				settings.randomMode = settings.random ? 1 : 0;
			}
			settings.random = settings.randomMode != 0;
			if (keepVolume) {
				settings.volume = keptVolume;
			}
			EnsureMenuIni();
			EnsureRandomIni();
			EnsureAudioIni();
		}

		void WriteVolume()
		{
			std::ifstream input(IniPath());
			std::vector<std::string> lines;
			bool found = false;
			std::string line;
			while (std::getline(input, line)) {
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				auto probe = line;
				const auto comment = probe.find_first_of(";#");
				if (comment != std::string::npos) {
					probe.erase(comment);
				}
				if (probe.find("fVolume") != std::string::npos && probe.find("fVolumeStep") == std::string::npos) {
					char buffer[32]{};
					std::snprintf(buffer, sizeof(buffer), "fVolume=%.2f", settings.volume);
					line = buffer;
					found = true;
				}
				lines.push_back(line);
			}
			if (!found) {
				char buffer[32]{};
				std::snprintf(buffer, sizeof(buffer), "fVolume=%.2f", settings.volume);
				lines.emplace_back(buffer);
			}
			std::ofstream output(IniPath(), std::ios::binary);
			for (const auto& item : lines) {
				output << item << "\r\n";
			}
		}

		void WriteDefaultIni() const
		{
			std::ofstream file(IniPath(), std::ios::binary);
			if (!file) {
				return;
			}
			file <<
				"; Loading Screens\r\n"
				"; Images and video: SKSE\\Plugins\\LoadingScreens\\\r\n"
				"; Emergency clips: SKSE\\Plugins\\LoadingScreens\\Emergency\\\r\n"
				"; Only file names are listed. One clip plays at a time, even if the folder holds many gigabytes.\r\n"
				"; Images: png, jpg, jpeg, webp, bmp, gif, tga\r\n"
				"; Video: mp4, m4v, mov, avi, wmv, mkv, webm, flv, mpeg\r\n"
				"; Windows plays mp4, m4v, mov, avi, and wmv. mkv, webm, and hevc depend on system codecs.\r\n"
				"; bEmergencyLoop=true repeats the emergency clip. false starts the next clip when it ends.\r\n"
				"\r\n"
				"[General]\r\n"
				"bEnabled=true\r\n"
				"; 0 play in folder order, 1 random each time, 2 shuffle with no repeats until every file was shown.\r\n"
				"iRandomMode=2\r\n"
				"bEmergencyLoop=true\r\n"
				"iEmergencyKey=35\r\n"
				"iFitMode=0\r\n"
				"; Sound from the loading-screen clip itself.\r\n"
				"bPlayAudio=true\r\n"
				"; When bPlayAudio=false, play a random track from sLoadingAudioFolder instead.\r\n"
				"bLoadingFolderAudio=false\r\n"
				"; Folder relative to SKSE\\Plugins\\LoadingScreens, or a full path. mp3, wav, m4a, flac, wma.\r\n"
				"sLoadingAudioFolder=Audio\r\n"
				"fVolume=0.20\r\n"
				"fVolumeStep=0.10\r\n"
				"\r\n"
				"[Hotkeys]\r\n"
				"; https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes\r\n"
				"; -1 disables the key. Tab = 9, PageUp = 33, PageDown = 34, End = 35.\r\n"
				"; Pressing End again returns to the normal list.\r\n"
				"iPlayNextKey=9\r\n"
				"iVolumeUpKey=33\r\n"
				"iVolumeDownKey=34\r\n"
				"\r\n"
				"[MainMenu]\r\n"
				"; Background under the main menu buttons. Off by default, so the menu stays as it is.\r\n"
				"bMainMenuVideo=false\r\n"
				"; true uses the same folder as the loading screens. false uses the separate folder sMainMenuFolder.\r\n"
				"bMainMenuSharedFolder=true\r\n"
				"; Folder relative to SKSE\\Plugins, or Data\\MainMenuVideo, or a full path.\r\n"
				"; If the chosen folder is empty, Data\\MainMenuVideo is read as well.\r\n"
				"sMainMenuFolder=Data\\MainMenuVideo\r\n"
				"; Clip audio instead of the main menu music.\r\n"
				"bMainMenuAudio=true\r\n"
				"; 0 play once, 1 next file, 2 repeat the current file.\r\n"
				"iMainMenuPlayback=2\r\n";
		}

		void EnsureMenuIni()
		{
			std::ifstream input(IniPath());
			std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
			if (contents.find("bMainMenuVideo") != std::string::npos) {
				return;
			}
			std::ofstream output(IniPath(), std::ios::binary | std::ios::app);
			if (!output) {
				return;
			}
			output <<
				"\r\n"
				"[MainMenu]\r\n"
				"; Background under the main menu buttons. Off by default, so the menu stays as it is.\r\n"
				"bMainMenuVideo=false\r\n"
				"; true uses the same folder as the loading screens. false uses the separate folder sMainMenuFolder.\r\n"
				"bMainMenuSharedFolder=true\r\n"
				"; Folder relative to SKSE\\Plugins, or Data\\MainMenuVideo, or a full path.\r\n"
				"; If the chosen folder is empty, Data\\MainMenuVideo is read as well.\r\n"
				"sMainMenuFolder=Data\\MainMenuVideo\r\n"
				"; Clip audio instead of the main menu music.\r\n"
				"bMainMenuAudio=true\r\n"
				"; 0 play once, 1 next file, 2 repeat the current file.\r\n"
				"iMainMenuPlayback=2\r\n";
		}

		void EnsureRandomIni()
		{
			std::ifstream input(IniPath());
			std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
			if (contents.find("iRandomMode") != std::string::npos) {
				return;
			}
			std::ofstream output(IniPath(), std::ios::binary | std::ios::app);
			if (!output) {
				return;
			}
			output <<
				"\r\n"
				"; 0 play in folder order, 1 random each time, 2 shuffle with no repeats until every file was shown.\r\n"
				"iRandomMode=" << settings.randomMode << "\r\n";
		}

		void EnsureAudioIni()
		{
			std::ifstream input(IniPath());
			std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
			if (contents.find("bLoadingFolderAudio") != std::string::npos) {
				return;
			}
			const auto mark = contents.find("bPlayAudio=");
			std::ofstream output(IniPath(), std::ios::binary);
			if (!output) {
				return;
			}
			const char* block =
				"; When bPlayAudio=false, play a random track from sLoadingAudioFolder instead.\r\n"
				"bLoadingFolderAudio=false\r\n"
				"; Folder relative to SKSE\\Plugins\\LoadingScreens, or a full path. mp3, wav, m4a, flac, wma.\r\n"
				"sLoadingAudioFolder=Audio\r\n";
			if (mark == std::string::npos) {
				output << contents;
				if (!contents.empty() && contents.back() != '\n') {
					output << "\r\n";
				}
				output << block;
				return;
			}
			auto lineEnd = contents.find('\n', mark);
			if (lineEnd == std::string::npos) {
				lineEnd = contents.size();
			} else {
				++lineEnd;
			}
			output << contents.substr(0, lineEnd);
			output << block;
			output << contents.substr(lineEnd);
		}

		void RefreshAudioLibrary()
		{
			std::error_code error;
			const auto directory = AudioDir();
			std::filesystem::create_directories(directory, error);
			const auto stamp = std::filesystem::last_write_time(directory, error);
			{
				std::lock_guard lock(libraryMutex);
				if (!error && audioLibrary.ready && stamp == audioLibrary.stamp) {
					return;
				}
			}
			std::vector<std::filesystem::path> files;
			WIN32_FIND_DATAW data{};
			const auto pattern = (directory / L"*").wstring();
			HANDLE handle = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
			if (handle != INVALID_HANDLE_VALUE) {
				do {
					if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
						continue;
					}
					std::filesystem::path path = directory / data.cFileName;
					if (Playback::IsAudioFile(path)) {
						files.push_back(std::move(path));
					}
				} while (FindNextFileW(handle, &data));
				FindClose(handle);
			}
			std::sort(files.begin(), files.end());
			std::lock_guard lock(libraryMutex);
			const bool changed = audioLibrary.files != files;
			audioLibrary.files = std::move(files);
			audioLibrary.stamp = stamp;
			audioLibrary.ready = !error;
			if (changed) {
				audioLibrary.deck.clear();
				audioLibrary.deckPos = 0;
				audioIndex = static_cast<std::size_t>(-1);
			}
		}

		static float ParseUnit(const std::string& value)
		{
			float number = std::strtof(value.c_str(), nullptr);
			if (number > 1.0f) {
				number /= 100.0f;
			}
			return std::clamp(number, 0.0f, 1.0f);
		}

		static bool Pressed(int key, bool& wasDown)
		{
			if (key <= 0) {
				wasDown = false;
				return false;
			}
			const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
			const bool pressed = down && !wasDown;
			wasDown = down;
			return pressed;
		}

		void ResetKeys()
		{
			emergencyDown = false;
			nextDown = false;
			upDown = false;
			downDown = false;
			pendingAction = 0;
		}

		static std::filesystem::path DllDir()
		{
			wchar_t buffer[MAX_PATH]{};
			HMODULE module = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&DllDir), &module);
			GetModuleFileNameW(module, buffer, MAX_PATH);
			return std::filesystem::path(buffer).parent_path();
		}

		static std::filesystem::path MediaDir()
		{
			return DllDir() / "LoadingScreens";
		}

		static std::filesystem::path EmergencyDir()
		{
			return MediaDir() / "Emergency";
		}

		std::filesystem::path AudioDir() const
		{
			std::filesystem::path folder = settings.loadingAudioFolder.empty() ? "Audio" : settings.loadingAudioFolder;
			if (folder.is_absolute()) {
				return folder;
			}
			return MediaDir() / folder;
		}

		static std::filesystem::path IniPath()
		{
			return DllDir() / "LoadingScreens.ini";
		}

		static std::string Trim(std::string value)
		{
			const auto first = value.find_first_not_of(" \t");
			if (first == std::string::npos) {
				return {};
			}
			const auto last = value.find_last_not_of(" \t");
			return value.substr(first, last - first + 1);
		}

		static void LogBlob(ID3DBlob* errors)
		{
			if (!errors) {
				SKSE::log::error("Шейдер не собрался");
				return;
			}
			SKSE::log::error("{}", std::string_view(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
		}

		struct Pipeline
		{
			ComPtr<ID3D11VertexShader> vs;
			ComPtr<ID3D11PixelShader> ps;
			ComPtr<ID3D11Buffer> constants;
			ComPtr<ID3D11SamplerState> sampler;
			ComPtr<ID3D11BlendState> blend;
			ComPtr<ID3D11DepthStencilState> depth;
			ComPtr<ID3D11RasterizerState> raster;
			ComPtr<ID3D11VertexShader> spriteVs;
			ComPtr<ID3D11PixelShader> spritePs;
			ComPtr<ID3D11Buffer> spriteConstants;
			ComPtr<ID3D11BlendState> spriteBlend;
			ComPtr<ID3D11Texture2D> toastTexture;
			ComPtr<ID3D11ShaderResourceView> toastView;

			void Reset()
			{
				vs.Reset();
				ps.Reset();
				constants.Reset();
				sampler.Reset();
				blend.Reset();
				depth.Reset();
				raster.Reset();
				spriteVs.Reset();
				spritePs.Reset();
				spriteConstants.Reset();
				spriteBlend.Reset();
				toastTexture.Reset();
				toastView.Reset();
			}
		};

		struct Library
		{
			std::vector<std::filesystem::path> files;
			std::vector<std::size_t> deck;
			std::size_t deckPos = 0;
			std::filesystem::file_time_type stamp{};
			bool ready = false;
		};

		Settings settings;
		Pipeline pipeline;
		Playback playback;
		Library mainLibrary;
		Library emergencyLibrary;
		Library menuLibrary;
		Library audioLibrary;
		std::mutex libraryMutex;
		std::size_t mainIndex = 0;
		std::size_t emergencyIndex = 0;
		std::size_t menuIndex = 0;
		std::size_t audioIndex = static_cast<std::size_t>(-1);
		std::mt19937 rng{ std::random_device{}() };
		std::wstring toastText;
		std::chrono::steady_clock::time_point toastUntil{};
		bool toastDirty = false;
		int toastW = 0;
		int toastH = 0;
		bool wasLoading = false;
		bool menuSession = false;
		bool seenGame = false;
		bool quittingToMenu = false;
		bool foreignMenuVideo = false;
		bool musicHooked = false;
		bool emergencyLatched = false;
		bool emergencyDown = false;
		bool switching = false;
		int pendingAction = 0;
		std::chrono::steady_clock::time_point switchReady{};
		bool nextDown = false;
		bool upDown = false;
		bool downDown = false;
		bool started = false;
		bool listening = false;
		bool loggedFrame = false;
		bool loggedDevice = false;
		bool loggedTarget = false;
		bool loggedTexture = false;
		ID3D11Device* boundDevice = nullptr;
		static inline PostDisplayFn originalPostDisplay = nullptr;
		static inline MainPostFn originalMainPost = nullptr;
		static inline MusicFn originalMusicA = nullptr;
		static inline MusicFn originalMusicB = nullptr;
	};
}

using namespace REL::literals;

SKSEPluginInfo(
	.Version = "1.3.11"_v,
	.Name = "LoadingScreens",
	.Author = "",
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

void SetupLog()
{
	auto directory = SKSE::log::log_directory();
	if (!directory) {
		return;
	}
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*directory / "LoadingScreens.log").string(), true);
	auto logger = std::make_shared<spdlog::logger>("LoadingScreens", std::move(sink));
	logger->set_level(spdlog::level::info);
	logger->flush_on(spdlog::level::info);
	spdlog::set_default_logger(std::move(logger));
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
	SKSE::Init(skse);
	SKSE::AllocTrampoline(64);
	SetupLog();
	SKSE::log::info("Loading Screens 1.3.11");
	if (auto* messaging = SKSE::GetMessagingInterface()) {
		messaging->RegisterListener([](SKSE::MessagingInterface::Message* message) {
			if (message && (message->type == SKSE::MessagingInterface::kPostLoad || message->type == SKSE::MessagingInterface::kDataLoaded)) {
				Overlay::Get().Start();
			}
		});
	}
	return true;
}
