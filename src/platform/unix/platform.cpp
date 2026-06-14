#include "platform.h"

#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

void open_browser(const std::string& url) {
    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed while opening browser");
    }
    if (pid == 0) {
#if defined(__APPLE__)
        execlp("open", "open", url.c_str(), static_cast<char*>(nullptr));
#else
        execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char*>(nullptr));
#endif
        _exit(127);
    }
}
