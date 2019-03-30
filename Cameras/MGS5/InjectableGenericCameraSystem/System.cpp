﻿////////////////////////////////////////////////////////////////////////////////////////////////////////
// Part of Injectable Generic Camera System
// Copyright(c) 2017, Frans Bouma
// All rights reserved.
// https://github.com/FransBouma/InjectableGenericCameraSystem
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met :
//
//  * Redistributions of source code must retain the above copyright notice, this
//	  list of conditions and the following disclaimer.
//
//  * Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and / or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
////////////////////////////////////////////////////////////////////////////////////////////////////////
#include "stdafx.h"
#include "System.h"
#include "Globals.h"
#include "Defaults.h"
#include "GameConstants.h"
#include "Gamepad.h"
#include "CameraManipulator.h"
#include "InterceptorHelper.h"
#include "InputHooker.h"
#include "input.h"
#include "CameraManipulator.h"
#include "GameImageHooker.h"
#include "UniversalD3D11Hook.h"
#include "OverlayConsole.h"
#include "direct.h"
#include "time.h"
#include "OverlayControl.h"

namespace IGCS
{
	using namespace IGCS::GameSpecific;

	System::System()
	{
	}


	System::~System()
	{
	}


	void System::start(LPBYTE hostBaseAddress, DWORD hostImageSize)
	{
		Globals::instance().systemActive(true);
		_hostImageAddress = (LPBYTE)hostBaseAddress;
		_hostImageSize = hostImageSize;
		Globals::instance().gamePad().setInvertLStickY(CONTROLLER_Y_INVERT);
		Globals::instance().gamePad().setInvertRStickY(CONTROLLER_Y_INVERT);
		initialize();		// will block till camera is found
		mainLoop();
	}


	// Core loop of the system
	void System::mainLoop()
	{
		while (Globals::instance().systemActive())
		{
			Sleep(FRAME_SLEEP);
			updateFrame();
		}
	}


	// updates the data and camera for a frame 
	void System::updateFrame()
	{
		handleUserInput();
		writeNewCameraValuesToCameraStructs();
	}
	

	void System::writeNewCameraValuesToCameraStructs()
	{
		if (!g_cameraEnabled)
		{
			return;
		}

		// calculate new camera values. We have two cameras, but they might not be available both, so we have to test before we do anything. 
		XMVECTOR newLookQuaternion = _camera.calculateLookQuaternion();
		XMFLOAT3 currentCoords; 
		XMFLOAT3 newCoords;
		if (GameSpecific::CameraManipulator::isCameraFound())
		{
			currentCoords = GameSpecific::CameraManipulator::getCurrentCameraCoords();
			newCoords = _camera.calculateNewCoords(currentCoords, newLookQuaternion);
			GameSpecific::CameraManipulator::writeNewCameraValuesToGameData(newCoords, newLookQuaternion, false);
		}
		if (GameSpecific::CameraManipulator::isCutsceneCameraFound())
		{
			currentCoords = GameSpecific::CameraManipulator::getCurrentCutsceneCameraCoords();
			newCoords = _camera.calculateNewCoords(currentCoords, newLookQuaternion);
			GameSpecific::CameraManipulator::writeNewCameraValuesToGameData(newCoords, newLookQuaternion, true);
		}
	}


	void System::handleUserInput()
	{
		Globals::instance().gamePad().update();
		bool altPressed = Input::keyDown(VK_LMENU) || Input::keyDown(VK_RMENU);
		bool rcontrolPressed = Input::keyDown(VK_RCONTROL);
		bool lcontrolPressed = Input::keyDown(VK_LCONTROL);

		if (Input::keyDown(IGCS_KEY_TOGGLE_OVERLAY) && (lcontrolPressed || rcontrolPressed))
		{
			OverlayControl::toggleOverlay();
			Sleep(350);		// wait 100ms to avoid fast keyboard hammering
			// we're done now. 
			return;
		}
		if (!_cameraStructFound)
		{
			// camera not found yet, can't proceed.
			return;
		}
		if (OverlayControl::isMainMenuVisible() && !Globals::instance().settings().allowCameraMovementWhenMenuIsUp)
		{
			// stop here, so keys used in the camera system won't affect anything of the camera
			return;
		}
		if (Input::keyDown(IGCS_KEY_CAMERA_ENABLE))
		{
			if (g_cameraEnabled)
			{
				// it's going to be disabled, make sure things are alright when we give it back to the host
				CameraManipulator::restoreOriginalValuesAfterCameraDisable();
				toggleCameraMovementLockState(false);
				InterceptorHelper::toggleFoVCutsceneWriteState(_aobBlocks, true);	// enable writes
			}
			else
			{
				// it's going to be enabled, so cache the original values before we enable it so we can restore it afterwards
				InterceptorHelper::toggleFoVCutsceneWriteState(_aobBlocks, false);	// disable writes
				CameraManipulator::cacheOriginalValuesBeforeCameraEnable();
				_camera.resetAngles();
			}
			g_cameraEnabled = g_cameraEnabled == 0 ? (BYTE)1 : (BYTE)0;
			displayCameraState();
			Sleep(350);				// wait for 350ms to avoid fast keyboard hammering
		}
		if (Input::keyDown(IGCS_KEY_FOV_RESET) && Globals::instance().keyboardMouseControlCamera())
		{
			CameraManipulator::resetFoV();
		}
		if (Input::keyDown(IGCS_KEY_FOV_DECREASE) && Globals::instance().keyboardMouseControlCamera())
		{
			CameraManipulator::changeFoV(-Globals::instance().settings().fovChangeSpeed);
		}
		if (Input::keyDown(IGCS_KEY_FOV_INCREASE) && Globals::instance().keyboardMouseControlCamera())
		{
			CameraManipulator::changeFoV(Globals::instance().settings().fovChangeSpeed);
		}
		if (Input::keyDown(IGCS_KEY_TIMESTOP))
		{
			freezeGame();
			Sleep(350);				// wait for 350ms to avoid fast keyboard hammering
		}

		if (!g_cameraEnabled)
		{
			// camera is disabled. We simply disable all input to the camera movement, by returning now.
			return;
		}

		_camera.resetMovement();

		if (_isLightfieldCapturing)
		{
			if (framesToGrab == 0) {
				_isLightfieldCapturing = false;
				moveLightfield(-1, true, false);
				OverlayConsole::instance().logLine("Lightfield photo end.");
				//InterceptorHelper::toggleHudRenderState(_aobBlocks, _hudToggled);
			}
			else {
				// synchronize camera position with lightfield capture
				if (!_lightfieldHookInited) {
					OverlayConsole::instance().logLine("Lightfield photo begin.");
					startCapture(framesToGrab);
					_lightfieldHookInited = true;
				}
				else if (framesToGrab >= DX11Hooker::framesRemaining()) {
					--framesToGrab;
					moveLightfield(1, false, false);
					return;
				}
				DX11Hooker::syncFramesToGrab(framesToGrab);
			}
		}

		Settings& settings = Globals::instance().settings();
		float multiplier = altPressed ? settings.fastMovementMultiplier : rcontrolPressed ? settings.slowMovementMultiplier : 1.0f;
		if (Input::keyDown(IGCS_KEY_CAMERA_LOCK))
		{
			toggleCameraMovementLockState(!_cameraMovementLocked);
			Sleep(350);				// wait for 350ms to avoid fast keyboard hammering
		}

		if (Input::keyDown(IGCS_KEY_LIGHTFIELD_PHOTO))
		{
			takeLightfieldPhoto();
		}
		else if (Input::keyDown(IGCS_KEY_LIGHTFIELD_LEFT))
		{
			moveLightfield(-1, altPressed);
			Sleep(350);				// wait for 350ms to avoid fast keyboard hammering
		}
		else if (Input::keyDown(IGCS_KEY_LIGHTFIELD_RIGHT))
		{
			moveLightfield(1, altPressed);
			Sleep(350);				// wait for 350ms to avoid fast keyboard hammering
		}
		if (_cameraMovementLocked)
		{
			// no movement allowed, simply return
			return;
		}

		handleKeyboardCameraMovement(multiplier);
		handleMouseCameraMovement(multiplier);
		handleGamePadMovement(multiplier);
	}


	void System::handleGamePadMovement(float multiplierBase)
	{
		if(!Globals::instance().controllerControlsCamera())
		{
			return;
		}

		auto gamePad = Globals::instance().gamePad();

		if (gamePad.isConnected())
		{
			Settings& settings = Globals::instance().settings();
			float  multiplier = gamePad.isButtonPressed(IGCS_BUTTON_FASTER) ? settings.fastMovementMultiplier 
																			: gamePad.isButtonPressed(IGCS_BUTTON_SLOWER) ? settings.slowMovementMultiplier : multiplierBase;
			vec2 rightStickPosition = gamePad.getRStickPosition();
			_camera.pitch(rightStickPosition.y * multiplier);
			_camera.yaw(rightStickPosition.x * multiplier);

			vec2 leftStickPosition = gamePad.getLStickPosition();
			_camera.moveUp((gamePad.getLTrigger() - gamePad.getRTrigger()) * multiplier);
			_camera.moveForward(leftStickPosition.y * multiplier);
			_camera.moveRight(leftStickPosition.x * multiplier);

			if (gamePad.isButtonPressed(IGCS_BUTTON_TILT_LEFT))
			{
				_camera.roll(multiplier);
			}
			if (gamePad.isButtonPressed(IGCS_BUTTON_TILT_RIGHT))
			{
				_camera.roll(-multiplier);
			}
			if (gamePad.isButtonPressed(IGCS_BUTTON_RESET_FOV))
			{
				CameraManipulator::resetFoV();
			}
			if (gamePad.isButtonPressed(IGCS_BUTTON_FOV_DECREASE))
			{
				CameraManipulator::changeFoV(-Globals::instance().settings().fovChangeSpeed);
			}
			if (gamePad.isButtonPressed(IGCS_BUTTON_FOV_INCREASE))
			{
				CameraManipulator::changeFoV(Globals::instance().settings().fovChangeSpeed);
			}
		}
	}


	void System::handleMouseCameraMovement(float multiplier)
	{
		if (!Globals::instance().keyboardMouseControlCamera())
		{
			return;
		}
		long mouseDeltaX = Input::getMouseDeltaX();
		long mouseDeltaY = Input::getMouseDeltaY();
		if (abs(mouseDeltaY) > 1)
		{
			_camera.pitch(-(static_cast<float>(mouseDeltaY) * MOUSE_SPEED_CORRECTION * multiplier));
		}
		if (abs(mouseDeltaX) > 1)
		{
			_camera.yaw(static_cast<float>(mouseDeltaX) * MOUSE_SPEED_CORRECTION * multiplier);
		}
	}


	void System::handleKeyboardCameraMovement(float multiplier)
	{
		if (!Globals::instance().keyboardMouseControlCamera())
		{
			return;
		}
		if (Input::keyDown(IGCS_KEY_MOVE_FORWARD))
		{
			_camera.moveForward(multiplier);
		}
		if (Input::keyDown(IGCS_KEY_MOVE_BACKWARD))
		{
			_camera.moveForward(-multiplier);
		}
		if (Input::keyDown(IGCS_KEY_MOVE_RIGHT))
		{
			_camera.moveRight(multiplier);
		}
		if (Input::keyDown(IGCS_KEY_MOVE_LEFT))
		{
			_camera.moveRight(-multiplier);
		}
		if (Input::keyDown(IGCS_KEY_MOVE_UP))
		{
			_camera.moveUp(multiplier);
		}
		if (Input::keyDown(IGCS_KEY_MOVE_DOWN))
		{
			_camera.moveUp(-multiplier);
		}
		if (Input::keyDown(IGCS_KEY_ROTATE_DOWN))
		{
			_camera.pitch(-multiplier);
		}
		if (Input::keyDown(IGCS_KEY_ROTATE_UP))
		{
			_camera.pitch(multiplier);
		}
		if (Input::keyDown(IGCS_KEY_ROTATE_RIGHT))
		{
			_camera.yaw(multiplier);
		}
		if (Input::keyDown(IGCS_KEY_ROTATE_LEFT))
		{
			_camera.yaw(-multiplier);
		}
		if (Input::keyDown(IGCS_KEY_TILT_LEFT))
		{
			_camera.roll(multiplier);
		}
		if (Input::keyDown(IGCS_KEY_TILT_RIGHT))
		{
			_camera.roll(-multiplier);
		}
	}


	// Initializes system. Will block till camera struct is found.
	void System::initialize()
	{
		Globals::instance().mainWindowHandle(Utils::findMainWindow(GetCurrentProcessId()));
		InputHooker::setInputHooks();
		DX11Hooker::initializeHook();
		Input::registerRawInput();

		GameSpecific::InterceptorHelper::initializeAOBBlocks(_hostImageAddress, _hostImageSize, _aobBlocks);
		GameSpecific::InterceptorHelper::setCameraStructInterceptorHook(_aobBlocks);
		waitForCameraStructAddresses();		// blocks till camera is found.
		GameSpecific::InterceptorHelper::setPostCameraStructHooks(_aobBlocks, _hostImageAddress);

		// camera struct found, init our own camera object now and hook into game code which uses camera.
		_cameraStructFound = true;
		_camera.setPitch(INITIAL_PITCH_RADIANS);
		_camera.setRoll(INITIAL_ROLL_RADIANS);
		_camera.setYaw(INITIAL_YAW_RADIANS);
	}


	// Waits for the interceptor to pick up the camera struct address. Should only return if address is found 
	void System::waitForCameraStructAddresses()
	{
		OverlayConsole::instance().logLine("Waiting for camera struct interception...");
		while(!GameSpecific::CameraManipulator::isCameraFound())
		{
			handleUserInput();
			Sleep(100);
		}
		OverlayControl::addNotification("Camera found.");
		GameSpecific::CameraManipulator::displayCameraStructAddress();
	}
		

	void System::toggleCameraMovementLockState(bool newValue)
	{
		if (_cameraMovementLocked == newValue)
		{
			// already in this state. Ignore
			return;
		}
		_cameraMovementLocked = newValue;
		OverlayControl::addNotification(_cameraMovementLocked ? "Camera movement is locked" : "Camera movement is unlocked");
	}


	void System::displayCameraState()
	{
		OverlayControl::addNotification(g_cameraEnabled ? "Camera enabled" : "Camera disabled");
	}

	// Freezes the game by using the menu timestop.
	void System::freezeGame(bool freeze)
	{
		bool gamePaused = CameraManipulator::setTimeStopValue(freeze);
		if (gamePaused)
		{
			OverlayControl::addNotification("Game paused. Unpause with the menu by pressing ESC twice.");
		}
		gameFrozen = gamePaused;
	}
	void System::freezeGame() { System::freezeGame(true); }
	int direxists(const char* path) {
		struct stat info;
		if (stat(path, &info) != 0)
			return 0;
		else if (info.st_mode & S_IFDIR)
			return 1;
		else
			return 0;
	}
	void System::takeLightfieldPhoto()
	{
		if (!direxists(Globals::instance().settings().screenshotDirectory))
		{
			OverlayConsole::instance().logError("Screenshot target directory does not exist!");
			return;
		}
		if (_isLightfieldCapturing || !DX11Hooker::isDoneSavingImages()) {
			OverlayConsole::instance().logError("Previous capture not complete!");
			return;
		}
		//InterceptorHelper::toggleHudRenderState(_aobBlocks, true);
		//CameraManipulator::setTimeStopValue(true);
		moveLightfield(-1, true, false);
		_lightfieldHookInited = false;
		framesToGrab = Globals::instance().settings().lkgViewCount;
		_isLightfieldCapturing = true;
	}
	void System::startCapture(int numViews = 1)
	{
		OverlayConsole::instance().logLine("Time stopped.");
		time_t t = time(nullptr);
		tm tm;
		localtime_s(&tm, &t);
		_screenshot_ts[0] = tm.tm_year + 1900;
		_screenshot_ts[1] = tm.tm_mon + 1;
		_screenshot_ts[2] = tm.tm_mday;
		_screenshot_ts[3] = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
		const int hour = _screenshot_ts[3] / 3600;
		const int minute = (_screenshot_ts[3] - hour * 3600) / 60;
		const int seconds = _screenshot_ts[3] - hour * 3600 - minute * 60;
		char buf[500];
		char* optional_backslash = "";
		char* screenshot_dir = Globals::instance().settings().screenshotDirectory;
		if (screenshot_dir[strlen(screenshot_dir) - 1] != '\\') {
			optional_backslash = "\\";
		}
		sprintf(buf, "%s%s%.4d-%.2d-%.2d-%.2d-%.2d-%.2d", screenshot_dir, optional_backslash, _screenshot_ts[0], _screenshot_ts[1], _screenshot_ts[2], hour, minute, seconds);
		mkdir(buf);
		DX11Hooker::takeScreenshot(buf, numViews);
	}
	void System::moveLightfield(int direction, bool end)
	{
		moveLightfield(direction, end, true);
	}
	void System::moveLightfield(int direction, bool end, bool log)
	{
		OverlayControl::addNotification("moveLightfield called");
		log = !log;
		if (end) {
			if (log) {
				//OverlayConsole::instance().logLine((direction > 0) ? "Move to end." : "Move to start.");
				OverlayControl::addNotification((direction > 0) ? "Move to end." : "Move to start.");
			}
			float dist = direction * 0.5f*(Globals::instance().settings().lkgViewDistance)*(Globals::instance().settings().lkgViewCount);
			_camera.moveRight(dist);
			return;
		}
		if (log) {
			//OverlayConsole::instance().logLine((direction > 0) ? "Move to next." : "Move to previous.");
			OverlayControl::addNotification((direction > 0) ? "Move to next." : "Move to previous.");
		}
		float dist = direction * (Globals::instance().settings().lkgViewDistance);
		_camera.moveRight(dist);
	}
}