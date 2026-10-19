#ifndef EMULATOR_INCLUDE_EMULATOR_XINPUT_H_
#define EMULATOR_INCLUDE_EMULATOR_XINPUT_H_

#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#include <xinput.h>
#else
// Minimal XInput-like definitions for non-Windows (compile-time stubs)
using DWORD = uint32_t;
using WORD  = uint16_t;

struct XINPUT_GAMEPAD {
	uint16_t wButtons;
	uint8_t  bLeftTrigger;
	uint8_t  bRightTrigger;
	int16_t  sThumbLX;
	int16_t  sThumbLY;
	int16_t  sThumbRX;
	int16_t  sThumbRY;
};

struct XINPUT_STATE {
	DWORD      dwPacketNumber;
	XINPUT_GAMEPAD Gamepad;
};

struct XINPUT_VIBRATION {
	WORD wLeftMotorSpeed;
	WORD wRightMotorSpeed;
};

constexpr DWORD ERROR_SUCCESS = 0;
constexpr DWORD ERROR_DEVICE_NOT_CONNECTED = 1167;

constexpr WORD XINPUT_GAMEPAD_DPAD_UP        = 0x0001;
constexpr WORD XINPUT_GAMEPAD_DPAD_DOWN      = 0x0002;
constexpr WORD XINPUT_GAMEPAD_DPAD_LEFT      = 0x0004;
constexpr WORD XINPUT_GAMEPAD_DPAD_RIGHT     = 0x0008;
constexpr WORD XINPUT_GAMEPAD_START          = 0x0010;
constexpr WORD XINPUT_GAMEPAD_BACK           = 0x0020;
constexpr WORD XINPUT_GAMEPAD_LEFT_THUMB     = 0x0040;
constexpr WORD XINPUT_GAMEPAD_RIGHT_THUMB    = 0x0080;
constexpr WORD XINPUT_GAMEPAD_LEFT_SHOULDER  = 0x0100;
constexpr WORD XINPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200;
constexpr WORD XINPUT_GAMEPAD_GUIDE          = 0x0400;
constexpr WORD XINPUT_GAMEPAD_A              = 0x1000;
constexpr WORD XINPUT_GAMEPAD_B              = 0x2000;
constexpr WORD XINPUT_GAMEPAD_X              = 0x4000;
constexpr WORD XINPUT_GAMEPAD_Y              = 0x8000;

constexpr int XUSER_MAX_COUNT = 4;

// Stub: XInput not available on non-Windows platforms
inline DWORD XInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState) {
	(void)dwUserIndex;
	(void)pState;
	return ERROR_DEVICE_NOT_CONNECTED;
}
inline DWORD XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration) {
	(void)dwUserIndex;
	(void)pVibration;
	return ERROR_DEVICE_NOT_CONNECTED;
}
#endif

#endif // EMULATOR_INCLUDE_EMULATOR_XINPUT_H_
