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
#endif

void os::process::ShowMessage(std::string_view message)
{
#if defined(__PROSPERO__)
    NotificationRequest request{};
    size_t length = std::min(message.size(), sizeof(request.message) - 1);
    memcpy(request.message, message.data(), length);
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
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
#else
    _exit(code);
#endif
}
