#include "stdafx.h"
#include <d3d11.h> 
#include "UniversalD3D11Hook.h"
#include "Console.h"
#include "MinHook.h"
#include "Globals.h"
#include "imgui.h"
#include "Defaults.h"
#include "imgui_impl_dx11.h"
#include "OverlayControl.h"
#include "OverlayConsole.h"
#include "Input.h"
#include <atomic>
#include <time.h>
#include <filesystem>
#include "stb_image.h"
//#include "stb_image_dds.h"
#include "stb_image_write.h"
#include "stb_image_resize.h"

#pragma comment(lib, "d3d11.lib")

namespace IGCS::DX11Hooker
{
	#define DXGI_PRESENT_INDEX			8
	#define DXGI_RESIZEBUFFERS_INDEX	13

	//--------------------------------------------------------------------------------------------------------------------------------
	// Forward declarations
	void createRenderTarget(IDXGISwapChain* pSwapChain);
	void cleanupRenderTarget();
	void initImGui();
	void initImGuiStyle();

	//--------------------------------------------------------------------------------------------------------------------------------
	// Typedefs of functions to hook
	typedef HRESULT(__stdcall *D3D11PresentHook) (IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
	typedef HRESULT(__stdcall *D3D11ResizeBuffersHook) (IDXGISwapChain* pSwapChain, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT newFormat, UINT swapChainFlags);

	static ID3D11Device* _device = nullptr;
	static ID3D11DeviceContext* _context = nullptr;
	static ID3D11RenderTargetView* _mainRenderTargetView = nullptr;
	IDXGISwapChain* pSwapChain;
	UINT _width;
	UINT _height;
	DXGI_FORMAT _backbuffer_format;

	//--------------------------------------------------------------------------------------------------------------------------------
	// Pointers to the original hooked functions
	static D3D11PresentHook hookedD3D11Present = nullptr;
	static D3D11ResizeBuffersHook hookedD3D11ResizeBuffers = nullptr;

	static bool _tmpSwapChainInitialized = false;
	static atomic_bool _imGuiInitializing = false;
	static atomic_bool _initializeDeviceAndContext = true;
	static atomic_bool _presentInProgress = false;

	HRESULT __stdcall detourD3D11ResizeBuffers(IDXGISwapChain* pSwapChain, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT newFormat, UINT swapChainFlags)
	{
		_imGuiInitializing = true;
		ImGui_ImplDX11_InvalidateDeviceObjects();
		cleanupRenderTarget();
		HRESULT toReturn = hookedD3D11ResizeBuffers(pSwapChain, bufferCount, width, height, newFormat, swapChainFlags);
		createRenderTarget(pSwapChain);
		ImGui_ImplDX11_CreateDeviceObjects();
		_imGuiInitializing = false;
		return toReturn;
	}


	HRESULT __stdcall detourD3D11Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
	{
		if (_presentInProgress)
		{
			return S_OK;
		}
		_presentInProgress = true;
		if (_tmpSwapChainInitialized)
		{
			if (!(Flags & DXGI_PRESENT_TEST) && !_imGuiInitializing)
			{
				if (_initializeDeviceAndContext)
				{
					if (FAILED(pSwapChain->GetDevice(__uuidof(_device), (void**)&_device)))
					{
						IGCS::Console::WriteError("Failed to get device from hooked swapchain");
					}
					else
					{
						OverlayConsole::instance().logDebug("DX11 Device: %p", (void*)_device);
						_device->GetImmediateContext(&_context);
					}
					if (nullptr == _context)
					{
						IGCS::Console::WriteError("Failed to get device context from hooked swapchain");
					}
					else
					{
						OverlayConsole::instance().logDebug("DX11 Context: %p", (void*)_context);
					}
					createRenderTarget(pSwapChain);
					initImGui();

					_initializeDeviceAndContext = false;
				}
				// render our own stuff
				_context->OMSetRenderTargets(1, &_mainRenderTargetView, NULL);
				OverlayControl::renderOverlay();
				Input::resetKeyStates();
				Input::resetMouseState();
			}

		}
		HRESULT toReturn = hookedD3D11Present(pSwapChain, SyncInterval, Flags);
		_presentInProgress = false;
		return toReturn;
	}

	void initializeHook()
	{
		_tmpSwapChainInitialized = false;
		D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
		DXGI_SWAP_CHAIN_DESC swapChainDesc;
		ZeroMemory(&swapChainDesc, sizeof(swapChainDesc));
		swapChainDesc.BufferCount = 1;
		swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapChainDesc.OutputWindow = IGCS::Globals::instance().mainWindowHandle();
		swapChainDesc.SampleDesc.Count = 1;
		swapChainDesc.Windowed = TRUE;
		swapChainDesc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
		swapChainDesc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
		swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		_width = swapChainDesc.BufferDesc.Width;
		_height = swapChainDesc.BufferDesc.Height;
		_backbuffer_format = swapChainDesc.BufferDesc.Format;

		ID3D11Device *pTmpDevice = NULL;
		ID3D11DeviceContext *pTmpContext = NULL;
		IDXGISwapChain* pTmpSwapChain;
		if (FAILED(D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, NULL, &featureLevel, 1, D3D11_SDK_VERSION, &swapChainDesc, &pTmpSwapChain, &pTmpDevice, NULL, &pTmpContext)))
		{
			IGCS::Console::WriteError("Failed to create directX device and swapchain!");
			return;
		}

		__int64* pSwapChainVtable = NULL;
		__int64* pDeviceContextVTable = NULL;
		pSwapChainVtable = (__int64*)pTmpSwapChain;
		pSwapChainVtable = (__int64*)pSwapChainVtable[0];
		pDeviceContextVTable = (__int64*)pTmpContext;
		pDeviceContextVTable = (__int64*)pDeviceContextVTable[0];

		OverlayConsole::instance().logDebug("Present Address: %p", (void*)(__int64*)pSwapChainVtable[DXGI_PRESENT_INDEX]);

		if (MH_CreateHook((LPBYTE)pSwapChainVtable[DXGI_PRESENT_INDEX], &detourD3D11Present, reinterpret_cast<LPVOID*>(&hookedD3D11Present)) != MH_OK)
		{
			IGCS::Console::WriteError("Hooking Present failed!");
		}
		if (MH_EnableHook((LPBYTE)pSwapChainVtable[DXGI_PRESENT_INDEX]) != MH_OK)
		{
			IGCS::Console::WriteError("Enabling of Present hook failed!");
		}

		OverlayConsole::instance().logDebug("ResizeBuffers Address: %p", (__int64*)pSwapChainVtable[DXGI_RESIZEBUFFERS_INDEX]);

		if (MH_CreateHook((LPBYTE)pSwapChainVtable[DXGI_RESIZEBUFFERS_INDEX], &detourD3D11ResizeBuffers, reinterpret_cast<LPVOID*>(&hookedD3D11ResizeBuffers)) != MH_OK)
		{
			IGCS::Console::WriteError("Hooking ResizeBuffers failed!");
		}
		if (MH_EnableHook((LPBYTE)pSwapChainVtable[DXGI_RESIZEBUFFERS_INDEX]) != MH_OK)
		{
			IGCS::Console::WriteError("Enabling of ResizeBuffers hook failed!");
		}

		pTmpDevice->Release();
		pTmpContext->Release();
		pTmpSwapChain->Release();
		_tmpSwapChainInitialized = true;

		OverlayConsole::instance().logDebug("DX11 hooks set");
	}


	void createRenderTarget(IDXGISwapChain* pSwapChain)
	{
		DXGI_SWAP_CHAIN_DESC sd;
		pSwapChain->GetDesc(&sd);
		ID3D11Texture2D* pBackBuffer;
		D3D11_RENDER_TARGET_VIEW_DESC render_target_view_desc;
		ZeroMemory(&render_target_view_desc, sizeof(render_target_view_desc));
		render_target_view_desc.Format = sd.BufferDesc.Format;
		render_target_view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
		_device->CreateRenderTargetView(pBackBuffer, &render_target_view_desc, &_mainRenderTargetView);
		pBackBuffer->Release();
	}


	void cleanupRenderTarget()
	{
		if (nullptr != _mainRenderTargetView)
		{
			_mainRenderTargetView->Release();
			_mainRenderTargetView = nullptr;
		}
	}


	void initImGui()
	{
		ImGui_ImplDX11_Init(IGCS::Globals::instance().mainWindowHandle(), _device, _context);
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = IGCS_OVERLAY_INI_FILENAME;
		initImGuiStyle();
	}
	void capture_frame(uint8_t *buffer)
	{
		D3D11_TEXTURE2D_DESC StagingDesc;
		ID3D11Texture2D *pBackBuffer = NULL;
		pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
		pBackBuffer->GetDesc(&StagingDesc);
		StagingDesc.Usage = D3D11_USAGE_STAGING;
		StagingDesc.BindFlags = 0;
		StagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		StagingDesc.Width = _width;
		StagingDesc.Height = _height;

		StagingDesc.ArraySize = 1;
		StagingDesc.MipLevels = 1;
		StagingDesc.Format = _backbuffer_format;
		StagingDesc.SampleDesc.Count = 1;
		StagingDesc.Usage = D3D11_USAGE_STAGING;
		StagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ID3D11Texture2D *pBackBufferStaging = NULL;
		HRESULT hr = _device->CreateTexture2D(&StagingDesc, NULL, &pBackBufferStaging);

		if (FAILED(hr))
		{
			std::cout << "Failed to create staging resource for screenshot capture! HRESULT is '" << std::hex << hr << std::dec << "'." << std::endl;
			return;
		}

		_context->CopyResource(pBackBufferStaging, pBackBuffer);
		D3D11_MAPPED_SUBRESOURCE mapped;
		hr = _context->Map(pBackBufferStaging, 0, D3D11_MAP_READ, 0, &mapped);

		if (FAILED(hr))
		{
			std::cout << "Failed to map staging resource with screenshot capture! HRESULT is '" << std::hex << hr << std::dec << "'." << std::endl;
			return;
		}

		auto mapped_data = static_cast<BYTE *>(mapped.pData);
		const UINT pitch = StagingDesc.Width * 4;

		for (UINT y = 0; y < StagingDesc.Height; y++)
		{
			memcpy(buffer, mapped_data, min(pitch, static_cast<UINT>(mapped.RowPitch)));

			for (UINT x = 0; x < pitch; x += 4)
			{
				buffer[x + 3] = 0xFF;

				if (StagingDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || StagingDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
				{
					std::swap(buffer[x + 0], buffer[x + 2]);
				}
			}
			buffer += pitch;
			mapped_data += mapped.RowPitch;
		}

		_context->Unmap(pBackBufferStaging, 0);
	}
	void takeScreenshot()
	{
		int _date[4] = {};
		time_t t = time(nullptr); tm tm;
		localtime_s(&tm, &t);
		_date[0] = tm.tm_year + 1900;
		_date[1] = tm.tm_mon + 1;
		_date[2] = tm.tm_mday;
		_date[3] = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
		std::vector<uint8_t> data(_width * _height * 4);
		capture_frame(data.data());
		const int hour = _date[3] / 3600;
		const int minute = (_date[3] - hour * 3600) / 60;
		const int seconds = _date[3] - hour * 3600 - minute * 60;

		wchar_t filename[86];
		wprintf_s(filename, "C:\\Users\\theka\\Desktop\\aco_screenshots\\%.4d-%.2d-%.2d %.2d-%.2d-%.2d.png", _date[0], _date[1], _date[2], hour, minute, seconds);
		std::wstring screenshot_path = filename;
		std::cout << "Saving screenshot to " << screenshot_path.c_str() << " ..." << std::endl;

		bool _screenshot_save_success = false; // Default to a save failure unless it is reported to succeed below

		if (FILE *file; _wfopen_s(&file, screenshot_path.c_str(), L"wb") == 0)
		{
			const auto write_callback = [](void *context, void *data, int size) {
				fwrite(data, 1, size, static_cast<FILE *>(context));
			};
			_screenshot_save_success = stbi_write_png_to_func(write_callback, file, _width, _height, 4, data.data(), 0) != 0;

			fclose(file);
		}

		if (!_screenshot_save_success)
		{
			std::cout << "Failed to write screenshot to " << screenshot_path.c_str() << '!' << std::endl;
		}
	}
	void initImGuiStyle()
	{
		ImGuiStyle& style = ImGui::GetStyle();

		style.WindowRounding = 2.0f;
		style.FrameRounding = 1.0f;
		style.IndentSpacing = 25.0f;
		style.ScrollbarSize = 16.0f;
		style.ScrollbarRounding = 1.0f;

		style.Colors[ImGuiCol_Text] = ImVec4(0.84f, 0.84f, 0.88f, 1.00f);
		style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.24f, 0.24f, 0.29f, 1.00f);
		style.Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.09f, 0.90f);
		style.Colors[ImGuiCol_ChildWindowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
		style.Colors[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
		style.Colors[ImGuiCol_Border] = ImVec4(0.80f, 0.80f, 0.83f, 0.88f);
		style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.92f, 0.91f, 0.88f, 0.00f);
		style.Colors[ImGuiCol_FrameBg] = ImVec4(0.22f, 0.22f, 0.24f, 0.31f);
		style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.24f, 0.25f, 1.00f);
		style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.35f, 0.35f, 0.38f, 1.00f);
		style.Colors[ImGuiCol_TitleBg] = ImVec4(0.27f, 0.27f, 0.33f, 0.37f);
		style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.40f, 0.40f, 0.80f, 0.20f);
		style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
		style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.37f, 0.37f, 0.42f, 0.42f);
		style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.09f, 0.09f, 0.10f, 1.00f);
		style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.45f, 0.45f, 0.45f, 0.30f);
		style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.59f, 0.59f, 0.59f, 1.00f);
		style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.59f, 0.59f, 0.59f, 1.00f);
		style.Colors[ImGuiCol_ComboBg] = ImVec4(0.13f, 0.13f, 0.16f, 1.00f);
		style.Colors[ImGuiCol_CheckMark] = ImVec4(0.80f, 0.80f, 0.83f, 0.53f);
		style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.65f, 0.31f, 0.00f, 0.71f);
		style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.59f, 0.59f, 0.59f, 1.00f);
		style.Colors[ImGuiCol_Button] = ImVec4(0.65f, 0.31f, 0.00f, 0.86f);
		style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.80f, 0.41f, 0.00f, 1.00f);
		style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		style.Colors[ImGuiCol_Header] = ImVec4(0.50f, 0.50f, 0.53f, 0.49f);
		style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.47f, 0.47f, 0.49f, 1.00f);
		style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.40f, 0.40f, 0.44f, 0.31f);
		style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.44f, 0.44f, 0.44f, 0.30f);
		style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.59f, 0.59f, 0.59f, 1.00f);
		style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.59f, 0.59f, 0.59f, 1.00f);
		style.Colors[ImGuiCol_CloseButton] = ImVec4(0.40f, 0.40f, 0.40f, 0.44f);
		style.Colors[ImGuiCol_CloseButtonHovered] = ImVec4(0.40f, 0.40f, 0.40f, 0.93f);
		style.Colors[ImGuiCol_CloseButtonActive] = ImVec4(0.40f, 0.39f, 0.38f, 1.00f);
		style.Colors[ImGuiCol_PlotLines] = ImVec4(0.40f, 0.39f, 0.38f, 0.63f);
		style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.25f, 1.00f, 0.00f, 1.00f);
		style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.40f, 0.39f, 0.38f, 0.63f);
		style.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.25f, 1.00f, 0.00f, 1.00f);
		style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.25f, 1.00f, 0.00f, 0.43f);
		style.Colors[ImGuiCol_ModalWindowDarkening] = ImVec4(1.00f, 0.98f, 0.95f, 0.73f);
	}
}