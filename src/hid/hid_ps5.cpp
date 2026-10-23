// PS5: player 1 is the initial user's DualSense, read through scePad. The button bits and
// the pad data layout follow UnleashedRecomp-Prospero's hid driver (GPL-3.0). The game
// polls from its own threads; rumble is applied from the main loop (hid::Update), as on PC.
#if defined(__PROSPERO__)

#include <stdafx.h>
#include <hid/hid.h>
#include <kernel/xdm.h>
#include <os/logger.h>

extern "C"
{
    int sceUserServiceInitialize(const void* params);
    int sceUserServiceGetInitialUser(int32_t* userId);
    int scePadInit(void);
    int scePadOpen(int32_t userId, int32_t type, int32_t index, const void* param);
    int scePadGetHandle(int32_t userId, int32_t type, int32_t index);
    int scePadReadState(int32_t handle, void* data);
    int scePadSetVibration(int32_t handle, const void* param);
}

namespace
{
    struct ScePadData
    {
        uint32_t buttons;
        uint8_t leftStickX;
        uint8_t leftStickY;
        uint8_t rightStickX;
        uint8_t rightStickY;
        uint8_t analogL2;
        uint8_t analogR2;
        uint16_t padding;
        float orientation[4];
        float acceleration[3];
        float angularVelocity[3];
        uint8_t touchData[32];
        uint8_t connected;
        uint64_t timestamp;
        uint8_t extension[64];
    };

    struct ScePadVibrationParam
    {
        uint8_t largeMotor;
        uint8_t smallMotor;
    };

    // ScePadData button bits.
    constexpr uint32_t PAD_L3 = 0x2, PAD_R3 = 0x4, PAD_OPTIONS = 0x8, PAD_UP = 0x10, PAD_RIGHT = 0x20, PAD_DOWN = 0x40, PAD_LEFT = 0x80,
        PAD_L2 = 0x100, PAD_R2 = 0x200, PAD_L1 = 0x400, PAD_R1 = 0x800, PAD_TRIANGLE = 0x1000, PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000,
        PAD_SQUARE = 0x8000, PAD_TOUCHPAD = 0x100000;

    std::mutex g_mutex;
    int g_userServiceResult = 0;
    int g_padInitResult = 0;
    int32_t g_handle = -1;
    uint32_t g_packetNumber = 0;
    bool g_rumblePending = false;
    XAMINPUT_VIBRATION g_rumble{};

    // The initial user's pad; if the user service doesn't say who that is, the user IDs
    // UnleashedRecomp-Prospero falls back to.
    void OpenPad()
    {
        int32_t user = -1;
        int userResult = sceUserServiceGetInitialUser(&user);
        std::vector<int32_t> users;
        if (userResult == 0 && user >= 0)
            users.push_back(user);
        for (int32_t fallback : { 1, 0, 0x10000000, 0xFF })
            if (std::find(users.begin(), users.end(), fallback) == users.end())
                users.push_back(fallback);

        int lastResult = 0;
        for (int32_t candidate : users)
        {
            int32_t handle = scePadOpen(candidate, 0, 0, nullptr);
            if (handle < 0)
                handle = scePadGetHandle(candidate, 0, 0);
            if (handle >= 0)
            {
                g_handle = handle;
                LOGFN("Input: DualSense of user 0x{:X} (handle {})", uint32_t(candidate), handle);
                return;
            }
            lastResult = handle;
        }

        static bool reported = false;
        if (!reported)
        {
            reported = true;
            LOGFN_ERROR("Input: no pad (user service 0x{:08X}, pad init 0x{:08X}, initial user 0x{:08X} = {}, last open 0x{:08X})",
                uint32_t(g_userServiceResult), uint32_t(g_padInitResult), uint32_t(userResult), user, uint32_t(lastResult));
        }
    }

    // A stick axis (0-255, 128 at rest) as XInput's (-32768-32767, up positive), with a
    // small dead zone.
    int16_t Axis(uint8_t raw, bool invert)
    {
        int32_t centred = int32_t(raw) - 128;
        if (std::abs(centred) <= 10)
            return 0;
        if (invert)
            centred = -centred - 1;
        return int16_t(std::clamp(centred * 256, -32768, 32767));
    }
}

void hid::Init()
{
    g_userServiceResult = sceUserServiceInitialize(nullptr);
    g_padInitResult = scePadInit();
    std::lock_guard lock(g_mutex);
    OpenPad();
}

uint32_t hid::GetState(uint32_t userIndex, XAMINPUT_STATE* state)
{
    if (state == nullptr)
        return ERROR_BAD_ARGUMENTS;
    memset(state, 0, sizeof(*state));
    if (userIndex != 0)
        return ERROR_DEVICE_NOT_CONNECTED;

    std::lock_guard lock(g_mutex);
    if (g_handle < 0)
        OpenPad();
    // (The state's connected flag isn't checked: UnleashedRecomp-Prospero doesn't rely on it.)
    ScePadData data{};
    int result = g_handle >= 0 ? scePadReadState(g_handle, &data) : -1;
    if (result != 0)
    {
        static bool reported = false;
        if (!reported && g_handle >= 0)
        {
            reported = true;
            LOGFN_ERROR("Input: scePadReadState failed (0x{:08X})", uint32_t(result));
        }
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    state->dwPacketNumber = g_packetNumber++;
    XAMINPUT_GAMEPAD& pad = state->Gamepad;
    const std::pair<uint32_t, uint16_t> buttons[] =
    {
        { PAD_UP, XAMINPUT_GAMEPAD_DPAD_UP }, { PAD_DOWN, XAMINPUT_GAMEPAD_DPAD_DOWN }, { PAD_LEFT, XAMINPUT_GAMEPAD_DPAD_LEFT },
        { PAD_RIGHT, XAMINPUT_GAMEPAD_DPAD_RIGHT }, { PAD_OPTIONS, XAMINPUT_GAMEPAD_START }, { PAD_TOUCHPAD, XAMINPUT_GAMEPAD_BACK },
        { PAD_L3, XAMINPUT_GAMEPAD_LEFT_THUMB }, { PAD_R3, XAMINPUT_GAMEPAD_RIGHT_THUMB }, { PAD_L1, XAMINPUT_GAMEPAD_LEFT_SHOULDER },
        { PAD_R1, XAMINPUT_GAMEPAD_RIGHT_SHOULDER }, { PAD_CROSS, XAMINPUT_GAMEPAD_A }, { PAD_CIRCLE, XAMINPUT_GAMEPAD_B },
        { PAD_SQUARE, XAMINPUT_GAMEPAD_X }, { PAD_TRIANGLE, XAMINPUT_GAMEPAD_Y },
    };
    uint16_t pressed = 0;
    for (auto [ps, xbox] : buttons)
        if ((data.buttons & ps) != 0)
            pressed |= xbox;
    pad.wButtons = pressed;
    pad.sThumbLX = Axis(data.leftStickX, false);
    pad.sThumbLY = Axis(data.leftStickY, true);
    pad.sThumbRX = Axis(data.rightStickX, false);
    pad.sThumbRY = Axis(data.rightStickY, true);
    pad.bLeftTrigger = data.analogL2 != 0 ? data.analogL2 : ((data.buttons & PAD_L2) != 0 ? 255 : 0);
    pad.bRightTrigger = data.analogR2 != 0 ? data.analogR2 : ((data.buttons & PAD_R2) != 0 ? 255 : 0);
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
    int32_t handle;
    {
        std::lock_guard lock(g_mutex);
        if (!g_rumblePending || g_handle < 0)
            return;
        g_rumblePending = false;
        rumble = g_rumble;
        handle = g_handle;
    }
    // XInput's left motor is the large (low-frequency) one.
    ScePadVibrationParam levels{ uint8_t(uint16_t(rumble.wLeftMotorSpeed) >> 8), uint8_t(uint16_t(rumble.wRightMotorSpeed) >> 8) };
    scePadSetVibration(handle, &levels);
}

void hid::OnFrame(uint64_t)
{
}

#endif
