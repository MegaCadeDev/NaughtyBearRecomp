// Controller and keyboard input through SDL. Adapted from Unleashed Recompiled's
// hid/driver/sdl_hid.cpp; the keyboard layout is new (see README).
//
// SDL delivers events on the main thread (the event watch below); the game polls from
// its own threads, so the state is shared under a mutex. Player 1 (user index 0) is the
// most recently used controller combined with the keyboard; other players report no
// controller connected.

#include <stdafx.h>
#include <hid/hid.h>
#include <kernel/xdm.h>
#include <os/logger.h>
#include <user/paths.h>
#include <SDL.h>

namespace
{
    constexpr uint16_t VIBRATION_TIMEOUT_MS = 5000;

    struct Controller
    {
        SDL_GameController* controller = nullptr;
        SDL_JoystickID id = -1;
        XAMINPUT_GAMEPAD state{};
    };

    std::mutex g_inputMutex;
    std::array<Controller, 4> g_controllers;
    Controller* g_activeController = nullptr;
    std::set<SDL_Scancode> g_keysDown;

    // Rumble requested by the game, applied on the main thread by hid::Update. The game's
    // threads must not call SDL's joystick functions while holding g_inputMutex: SDL
    // holds its joystick lock while it calls the event watch, which takes g_inputMutex.
    struct PendingRumble
    {
        bool pending = false;
        bool stopAll = false;
        SDL_JoystickID id = -1;
        uint16_t low = 0;
        uint16_t high = 0;
    };
    PendingRumble g_rumble;
    uint32_t g_packetNumber = 0;

    // NB_INPUT_SCRIPT="start@300+5,a@600+5": hold a button from a frame for a number of
    // frames. For automated testing without a person at the controls.
    struct ScriptedPress
    {
        uint16_t button;
        uint64_t frame;
        uint64_t length;
    };
    std::vector<ScriptedPress> g_script;
    std::atomic<uint64_t> g_frame = 0;

    void LoadInputScript()
    {
        const char* script = getenv("NB_INPUT_SCRIPT");
        if (script == nullptr)
            return;
        const std::pair<const char*, uint16_t> names[] =
        {
            { "a", XAMINPUT_GAMEPAD_A }, { "b", XAMINPUT_GAMEPAD_B }, { "x", XAMINPUT_GAMEPAD_X }, { "y", XAMINPUT_GAMEPAD_Y },
            { "start", XAMINPUT_GAMEPAD_START }, { "back", XAMINPUT_GAMEPAD_BACK }, { "lb", XAMINPUT_GAMEPAD_LEFT_SHOULDER },
            { "rb", XAMINPUT_GAMEPAD_RIGHT_SHOULDER }, { "up", XAMINPUT_GAMEPAD_DPAD_UP }, { "down", XAMINPUT_GAMEPAD_DPAD_DOWN },
            { "left", XAMINPUT_GAMEPAD_DPAD_LEFT }, { "right", XAMINPUT_GAMEPAD_DPAD_RIGHT },
        };
        std::string text(script);
        size_t start = 0;
        while (start < text.size())
        {
            size_t end = text.find(',', start);
            std::string item = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            start = end == std::string::npos ? text.size() : end + 1;

            size_t at = item.find('@'), plus = item.find('+');
            if (at == std::string::npos)
                continue;
            std::string name = item.substr(0, at);
            uint64_t frame = std::stoull(item.substr(at + 1, plus == std::string::npos ? std::string::npos : plus - at - 1));
            uint64_t length = plus == std::string::npos ? 5 : std::stoull(item.substr(plus + 1));
            for (auto [n, button] : names)
                if (name == n)
                    g_script.push_back({ button, frame, length });
        }
        LOGFN("Input: {} scripted presses", g_script.size());
    }

    Controller* FindController(SDL_JoystickID id)
    {
        for (auto& controller : g_controllers)
            if (controller.controller != nullptr && controller.id == id)
                return &controller;
        return nullptr;
    }

    void PollButtons(Controller& c)
    {
        struct Mapping { SDL_GameControllerButton sdl; uint16_t xam; };
        constexpr Mapping mappings[] =
        {
            { SDL_CONTROLLER_BUTTON_DPAD_UP, XAMINPUT_GAMEPAD_DPAD_UP },
            { SDL_CONTROLLER_BUTTON_DPAD_DOWN, XAMINPUT_GAMEPAD_DPAD_DOWN },
            { SDL_CONTROLLER_BUTTON_DPAD_LEFT, XAMINPUT_GAMEPAD_DPAD_LEFT },
            { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, XAMINPUT_GAMEPAD_DPAD_RIGHT },
            { SDL_CONTROLLER_BUTTON_START, XAMINPUT_GAMEPAD_START },
            { SDL_CONTROLLER_BUTTON_BACK, XAMINPUT_GAMEPAD_BACK },
            { SDL_CONTROLLER_BUTTON_TOUCHPAD, XAMINPUT_GAMEPAD_BACK },
            { SDL_CONTROLLER_BUTTON_LEFTSTICK, XAMINPUT_GAMEPAD_LEFT_THUMB },
            { SDL_CONTROLLER_BUTTON_RIGHTSTICK, XAMINPUT_GAMEPAD_RIGHT_THUMB },
            { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, XAMINPUT_GAMEPAD_LEFT_SHOULDER },
            { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, XAMINPUT_GAMEPAD_RIGHT_SHOULDER },
            { SDL_CONTROLLER_BUTTON_A, XAMINPUT_GAMEPAD_A },
            { SDL_CONTROLLER_BUTTON_B, XAMINPUT_GAMEPAD_B },
            { SDL_CONTROLLER_BUTTON_X, XAMINPUT_GAMEPAD_X },
            { SDL_CONTROLLER_BUTTON_Y, XAMINPUT_GAMEPAD_Y },
        };
        c.state.wButtons = 0;
        for (auto [sdl, xam] : mappings)
            if (SDL_GameControllerGetButton(c.controller, sdl))
                c.state.wButtons |= xam;
    }

    void PollAxes(Controller& c)
    {
        c.state.sThumbLX = SDL_GameControllerGetAxis(c.controller, SDL_CONTROLLER_AXIS_LEFTX);
        c.state.sThumbLY = ~SDL_GameControllerGetAxis(c.controller, SDL_CONTROLLER_AXIS_LEFTY);
        c.state.sThumbRX = SDL_GameControllerGetAxis(c.controller, SDL_CONTROLLER_AXIS_RIGHTX);
        c.state.sThumbRY = ~SDL_GameControllerGetAxis(c.controller, SDL_CONTROLLER_AXIS_RIGHTY);
        c.state.bLeftTrigger = SDL_GameControllerGetAxis(c.controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7;
        c.state.bRightTrigger = SDL_GameControllerGetAxis(c.controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7;
    }

    bool KeyDown(SDL_Scancode key)
    {
        return g_keysDown.count(key) != 0;
    }

    // Default keyboard layout (README lists it).
    void ApplyKeyboard(XAMINPUT_GAMEPAD& pad)
    {
        struct KeyButton { SDL_Scancode key; uint16_t button; };
        constexpr KeyButton buttons[] =
        {
            { SDL_SCANCODE_SPACE, XAMINPUT_GAMEPAD_A },
            { SDL_SCANCODE_E, XAMINPUT_GAMEPAD_B },
            { SDL_SCANCODE_F, XAMINPUT_GAMEPAD_X },
            { SDL_SCANCODE_R, XAMINPUT_GAMEPAD_Y },
            { SDL_SCANCODE_Q, XAMINPUT_GAMEPAD_LEFT_SHOULDER },
            { SDL_SCANCODE_TAB, XAMINPUT_GAMEPAD_RIGHT_SHOULDER },
            { SDL_SCANCODE_RETURN, XAMINPUT_GAMEPAD_START },
            { SDL_SCANCODE_ESCAPE, XAMINPUT_GAMEPAD_BACK },
            { SDL_SCANCODE_LSHIFT, XAMINPUT_GAMEPAD_LEFT_THUMB },
            { SDL_SCANCODE_V, XAMINPUT_GAMEPAD_RIGHT_THUMB },
            { SDL_SCANCODE_UP, XAMINPUT_GAMEPAD_DPAD_UP },
            { SDL_SCANCODE_DOWN, XAMINPUT_GAMEPAD_DPAD_DOWN },
            { SDL_SCANCODE_LEFT, XAMINPUT_GAMEPAD_DPAD_LEFT },
            { SDL_SCANCODE_RIGHT, XAMINPUT_GAMEPAD_DPAD_RIGHT },
        };
        for (auto [key, button] : buttons)
            if (KeyDown(key))
                pad.wButtons |= button;

        auto axis = [](bool negative, bool positive, int16_t current) -> int16_t
            {
                if (negative == positive)
                    return current;
                return positive ? 32767 : -32768;
            };
        pad.sThumbLX = axis(KeyDown(SDL_SCANCODE_A), KeyDown(SDL_SCANCODE_D), pad.sThumbLX);
        pad.sThumbLY = axis(KeyDown(SDL_SCANCODE_S), KeyDown(SDL_SCANCODE_W), pad.sThumbLY);
        pad.sThumbRX = axis(KeyDown(SDL_SCANCODE_J), KeyDown(SDL_SCANCODE_L), pad.sThumbRX);
        pad.sThumbRY = axis(KeyDown(SDL_SCANCODE_K), KeyDown(SDL_SCANCODE_I), pad.sThumbRY);
        if (KeyDown(SDL_SCANCODE_Z))
            pad.bLeftTrigger = 255;
        if (KeyDown(SDL_SCANCODE_C))
            pad.bRightTrigger = 255;
    }

    // Opens the controller at an SDL device index, unless it's open already.
    void OpenController(int deviceIndex)
    {
        if (!SDL_IsGameController(deviceIndex))
        {
            LOGFN_WARNING("Input: {} has no controller mapping (add one to gamecontrollerdb.txt)", SDL_JoystickNameForIndex(deviceIndex));
            return;
        }
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(deviceIndex);
        if (FindController(id) != nullptr)
            return;

        for (auto& slot : g_controllers)
        {
            if (slot.controller != nullptr)
                continue;
            slot.controller = SDL_GameControllerOpen(deviceIndex);
            if (slot.controller != nullptr)
            {
                slot.id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(slot.controller));
                slot.state = {};
                PollButtons(slot);
                PollAxes(slot);
                g_activeController = &slot;
                LOGFN("Input: controller connected: {}", SDL_GameControllerName(slot.controller));
            }
            break;
        }
    }

    // Mappings for controllers SDL's built-in database might lack, as SDL reports them on
    // Linux (xpad driver): the wired Xbox 360 controller (045e:028e), both firmware versions.
    // More can be added in a gamecontrollerdb.txt next to the executable or the game folder.
    constexpr const char* EXTRA_MAPPINGS[] =
    {
        "030000005e0400008e02000014010000,Xbox 360 Controller,a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b8,leftshoulder:b4,leftstick:b9,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b10,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b2,y:b3,platform:Linux,",
        "030000005e0400008e02000010010000,Xbox 360 Controller,a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b8,leftshoulder:b4,leftstick:b9,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b10,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b2,y:b3,platform:Linux,",
    };

    void LoadMappings()
    {
        for (const char* mapping : EXTRA_MAPPINGS)
            SDL_GameControllerAddMapping(mapping);

        std::vector<std::filesystem::path> files = { GetSavePath(false).parent_path() / "gamecontrollerdb.txt",
            std::filesystem::current_path() / "gamecontrollerdb.txt" };
        std::error_code ec;
        auto executable = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec)
            files.push_back(executable.parent_path() / "gamecontrollerdb.txt");
        for (const auto& file : files)
        {
            if (!std::filesystem::exists(file, ec))
                continue;
            int added = SDL_GameControllerAddMappingsFromFile(file.string().c_str());
            if (added >= 0)
                LOGFN("Input: {} controller mappings from {}", added, file.string());
            break;
        }
    }

    int OnSDLEvent(void*, SDL_Event* event)
    {
        std::lock_guard lock(g_inputMutex);
        switch (event->type)
        {
        case SDL_CONTROLLERDEVICEADDED:
            OpenController(event->cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (Controller* c = FindController(event->cdevice.which))
            {
                LOGFN("Input: controller disconnected: {}", SDL_GameControllerName(c->controller));
                SDL_GameControllerClose(c->controller);
                *c = {};
                if (g_activeController == c)
                {
                    g_activeController = nullptr;
                    for (auto& other : g_controllers)
                        if (other.controller != nullptr)
                            g_activeController = &other;
                }
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            // The Guide (Xbox) button closes the game.
            if (event->cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE && FindController(event->cbutton.which) != nullptr)
            {
                LOGN("Input: Guide button pressed, closing the game");
                SDL_Event quit{};
                quit.type = SDL_QUIT;
                SDL_PeepEvents(&quit, 1, SDL_ADDEVENT, 0, 0);
                break;
            }
            [[fallthrough]];
        case SDL_CONTROLLERBUTTONUP:
        case SDL_CONTROLLERAXISMOTION:
            if (Controller* c = FindController(event->cdevice.which))
            {
                if (event->type == SDL_CONTROLLERAXISMOTION)
                {
                    PollAxes(*c);
                    if (std::abs(event->caxis.value) > 8000)
                        g_activeController = c;
                }
                else
                {
                    PollButtons(*c);
                    g_activeController = c;
                }
            }
            break;
        case SDL_KEYDOWN:
            if (!event->key.repeat)
                g_keysDown.insert(event->key.keysym.scancode);
            break;
        case SDL_KEYUP:
            g_keysDown.erase(event->key.keysym.scancode);
            break;
        case SDL_WINDOWEVENT:
            if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
            {
                g_keysDown.clear();
                g_rumble = {};
                g_rumble.pending = true;
                g_rumble.stopAll = true;
            }
            break;
        }
        return 0;
    }
}

void hid::Init()
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_AddEventWatch(OnSDLEvent, nullptr);
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    LoadMappings();

    // Controllers connected before this point announced themselves before the event
    // watch was installed, so open them here; later ones arrive as events.
    {
        std::lock_guard lock(g_inputMutex);
        for (int i = 0; i < SDL_NumJoysticks(); i++)
            OpenController(i);
    }
    LoadInputScript();
}

void hid::Update()
{
    PendingRumble rumble;
    std::vector<SDL_JoystickID> open;
    {
        std::lock_guard lock(g_inputMutex);
        rumble = g_rumble;
        g_rumble.pending = false;
        if (rumble.pending && rumble.stopAll)
            for (const auto& c : g_controllers)
                if (c.controller != nullptr)
                    open.push_back(c.id);
    }
    if (!rumble.pending)
        return;

    // Controllers are only closed on this thread, so these stay valid here.
    if (rumble.stopAll)
    {
        for (SDL_JoystickID id : open)
            if (SDL_GameController* controller = SDL_GameControllerFromInstanceID(id))
                SDL_GameControllerRumble(controller, 0, 0, 0);
    }
    else if (SDL_GameController* controller = SDL_GameControllerFromInstanceID(rumble.id))
    {
        SDL_GameControllerRumble(controller, rumble.low, rumble.high, VIBRATION_TIMEOUT_MS);
    }
}

void hid::OnFrame(uint64_t frame)
{
    g_frame = frame;
}

uint32_t hid::GetState(uint32_t userIndex, XAMINPUT_STATE* state)
{
    if (state == nullptr)
        return ERROR_BAD_ARGUMENTS;
    memset(state, 0, sizeof(*state));
    if (userIndex != 0)
        return ERROR_DEVICE_NOT_CONNECTED;

    std::lock_guard lock(g_inputMutex);
    state->dwPacketNumber = g_packetNumber++;
    if (g_activeController != nullptr)
        state->Gamepad = g_activeController->state;
    ApplyKeyboard(state->Gamepad);
    uint64_t frame = g_frame;
    for (const auto& press : g_script)
        if (frame >= press.frame && frame < press.frame + press.length)
            state->Gamepad.wButtons |= press.button;
    return ERROR_SUCCESS;
}

uint32_t hid::SetState(uint32_t userIndex, XAMINPUT_VIBRATION* vibration)
{
    if (vibration == nullptr)
        return ERROR_BAD_ARGUMENTS;
    if (userIndex != 0)
        return ERROR_DEVICE_NOT_CONNECTED;

    std::lock_guard lock(g_inputMutex);
    if (g_activeController != nullptr)
    {
        g_rumble.pending = true;
        g_rumble.stopAll = false;
        g_rumble.id = g_activeController->id;
        g_rumble.low = vibration->wLeftMotorSpeed;
        g_rumble.high = vibration->wRightMotorSpeed;
    }
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
