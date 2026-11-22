// Switch 2: player 1 is the first controller (Joy-Con 2 pair, Pro Controller or handheld),
// read through libnx's pad API. Buttons follow their labels (A is the Xbox A button, Plus
// is Start, Minus is Back); ZL and ZR are the triggers. Rumble is applied from the main loop
// (hid::Update), as on PC. No USB: controllers come through HID only (see switch2/README.md).
#if defined(__SWITCH2__)

#include <stdafx.h>
#include <hid/hid.h>
#include <kernel/xdm.h>
#include <os/logger.h>
#include <os/switch2.h>

namespace
{
    std::mutex g_mutex;
    PadState g_pad;
    uint32_t g_packetNumber = 0;
    bool g_rumblePending = false;
    XAMINPUT_VIBRATION g_rumble{};

    // Two motors (left and right) for each way player 1 can hold the controllers.
    struct RumbleDevices
    {
        HidNpadIdType id;
        HidNpadStyleTag style;
        HidVibrationDeviceHandle handles[2];
        bool ready;
    };
    RumbleDevices g_rumbleDevices[] =
    {
        { HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld, {}, false },
        { HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey, {}, false },
        { HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual, {}, false },
    };

    // A stick axis (-32767-32767, up positive, as XInput's) with a small dead zone.
    int16_t Axis(int32_t raw)
    {
        if (std::abs(raw) <= 2000)
            return 0;
        return int16_t(std::clamp(raw, -32768, 32767));
    }
}

void hid::Init()
{
    std::lock_guard lock(g_mutex);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    for (auto& devices : g_rumbleDevices)
        devices.ready = R_SUCCEEDED(hidInitializeVibrationDevices(devices.handles, 2, devices.id, devices.style));
    LOGN("Input: player 1's controller (pad API)");
}

uint32_t hid::GetState(uint32_t userIndex, XAMINPUT_STATE* state)
{
    if (state == nullptr)
        return ERROR_BAD_ARGUMENTS;
    memset(state, 0, sizeof(*state));
    if (userIndex != 0)
        return ERROR_DEVICE_NOT_CONNECTED;

    std::lock_guard lock(g_mutex);
    padUpdate(&g_pad);
    if (!padIsConnected(&g_pad))
        return ERROR_DEVICE_NOT_CONNECTED;

    state->dwPacketNumber = g_packetNumber++;
    XAMINPUT_GAMEPAD& pad = state->Gamepad;
    uint64_t held = padGetButtons(&g_pad);
    const std::pair<uint64_t, uint16_t> buttons[] =
    {
        { HidNpadButton_Up, XAMINPUT_GAMEPAD_DPAD_UP }, { HidNpadButton_Down, XAMINPUT_GAMEPAD_DPAD_DOWN },
        { HidNpadButton_Left, XAMINPUT_GAMEPAD_DPAD_LEFT }, { HidNpadButton_Right, XAMINPUT_GAMEPAD_DPAD_RIGHT },
        { HidNpadButton_Plus, XAMINPUT_GAMEPAD_START }, { HidNpadButton_Minus, XAMINPUT_GAMEPAD_BACK },
        { HidNpadButton_StickL, XAMINPUT_GAMEPAD_LEFT_THUMB }, { HidNpadButton_StickR, XAMINPUT_GAMEPAD_RIGHT_THUMB },
        { HidNpadButton_L, XAMINPUT_GAMEPAD_LEFT_SHOULDER }, { HidNpadButton_R, XAMINPUT_GAMEPAD_RIGHT_SHOULDER },
        { HidNpadButton_A, XAMINPUT_GAMEPAD_A }, { HidNpadButton_B, XAMINPUT_GAMEPAD_B },
        { HidNpadButton_X, XAMINPUT_GAMEPAD_X }, { HidNpadButton_Y, XAMINPUT_GAMEPAD_Y },
    };
    uint16_t pressed = 0;
    for (auto [switchButton, xbox] : buttons)
        if ((held & switchButton) != 0)
            pressed |= xbox;
    pad.wButtons = pressed;
    HidAnalogStickState left = padGetStickPos(&g_pad, 0);
    HidAnalogStickState right = padGetStickPos(&g_pad, 1);
    pad.sThumbLX = Axis(left.x);
    pad.sThumbLY = Axis(left.y);
    pad.sThumbRX = Axis(right.x);
    pad.sThumbRY = Axis(right.y);
    pad.bLeftTrigger = (held & HidNpadButton_ZL) != 0 ? 255 : 0;
    pad.bRightTrigger = (held & HidNpadButton_ZR) != 0 ? 255 : 0;
    return ERROR_SUCCESS;
}

uint32_t hid::SetState(uint32_t userIndex, XAMINPUT_VIBRATION* vibration)
{
    if (vibration == nullptr)
        return ERROR_BAD_ARGUMENTS;
    if (userIndex != 0)
        return ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard lock(g_mutex);
    g_rumble = *vibration;
    g_rumblePending = true;
    return ERROR_SUCCESS;
}

uint32_t hid::GetCapabilities(uint32_t userIndex, XAMINPUT_CAPABILITIES* capabilities)
{
    if (capabilities == nullptr)
        return ERROR_BAD_ARGUMENTS;
    if (userIndex != 0)
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(capabilities, 0, sizeof(*capabilities));
    capabilities->Type = 1;    // XINPUT_DEVTYPE_GAMEPAD
    capabilities->SubType = 1; // XINPUT_DEVSUBTYPE_GAMEPAD
    capabilities->Gamepad.wButtons = 0xF3FF;
    capabilities->Gamepad.bLeftTrigger = 0xFF;
    capabilities->Gamepad.bRightTrigger = 0xFF;
    capabilities->Gamepad.sThumbLX = int16_t(0xFFC0);
    capabilities->Gamepad.sThumbLY = int16_t(0xFFC0);
    capabilities->Gamepad.sThumbRX = int16_t(0xFFC0);
    capabilities->Gamepad.sThumbRY = int16_t(0xFFC0);
    capabilities->Vibration.wLeftMotorSpeed = 0xFFFF;
    capabilities->Vibration.wRightMotorSpeed = 0xFFFF;
    return ERROR_SUCCESS;
}

void hid::Update()
{
    XAMINPUT_VIBRATION rumble;
    {
        std::lock_guard lock(g_mutex);
        if (!g_rumblePending)
            return;
        g_rumblePending = false;
        rumble = g_rumble;
    }
    // XInput's left motor is the large (low-frequency) one: the low band; the right, the high.
    HidVibrationValue value{};
    value.amp_low = float(uint16_t(rumble.wLeftMotorSpeed)) / 65535.0f;
    value.freq_low = 160.0f;
    value.amp_high = float(uint16_t(rumble.wRightMotorSpeed)) / 65535.0f;
    value.freq_high = 320.0f;
    for (auto& devices : g_rumbleDevices)
    {
        if (!devices.ready)
            continue;
        HidVibrationValue values[2] = { value, value };
        hidSendVibrationValues(devices.handles, values, 2);
    }
}

void hid::OnFrame(uint64_t)
{
}

#endif
