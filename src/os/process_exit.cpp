#include <stdafx.h>
#include <os/process.h>

#if defined(__PROSPERO__)
extern "C"
{
    // The notification request payloads use; only the message matters.
    struct NotificationRequest
    {
        char reserved[45];
        char message[3075];
    };
    int sceKernelSendNotificationRequest(int device, NotificationRequest* request, size_t size, int blocking);
    int sceSystemServiceLoadExec(const char* path, const char* const argv[]);
}
#elif defined(__SWITCH2__)
#include <os/switch2.h>
#endif

void os::process::ShowMessage(std::string_view message)
{
#if defined(__PROSPERO__)
    NotificationRequest request{};
    size_t length = std::min(message.size(), sizeof(request.message) - 1);
    memcpy(request.message, message.data(), length);
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
#elif defined(__SWITCH2__)
    // The system's error screen, which waits for the player to close it.
    ErrorApplicationConfig config;
    std::string text(message);
    if (R_SUCCEEDED(errorApplicationCreate(&config, text.c_str(), nullptr)))
        errorApplicationShow(&config);
#else
    (void)message;
#endif
}

void os::process::Exit(int code)
{
    fflush(nullptr);
#if defined(__PROSPERO__)
    // The shell closes the title asynchronously once it accepts the request (as
    // UnleashedRecomp-Prospero's App::Exit).
    (void)code;
    if (sceSystemServiceLoadExec("exit", nullptr) >= 0)
    {
        for (;;)
            usleep(100000);
    }
    // Refused: there's no other clean way out; stay idle rather than raise SIGSYS.
    for (;;)
        sleep(1);
#elif defined(__SWITCH2__)
    // The whole process ends, back to the HOME Menu. Returning to hbmenu (exit()) would leave
    // the game's threads running in the same process, over the next program. The SD card's
    // files are closed first so their last writes are kept.
    (void)code;
    fsdevUnmountAll();
    svcExitProcess();
#else
    _exit(code);
#endif
}
