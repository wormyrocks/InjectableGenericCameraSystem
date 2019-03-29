#pragma once

#include "stdafx.h"
#include <d3d11.h> 

namespace IGCS::DX11Hooker
{
	void initializeHook();
	void takeScreenshot(char* filename);
	void screenshotProcess(IDXGISwapChain* pSwapChain);
}