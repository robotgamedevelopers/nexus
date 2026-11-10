// A stand-in for ssh in the terminal functional test, built as a program named `ssh`: it connects nowhere. It acts as a
// remote shell would: it sets the terminal's title (OSC 0 and OSC 2), prints its arguments, then for each line it reads
// sets the title to that line, as vim or a remote prompt does, until `exit`, when it prints that the connection closed
// and ends, leaving its last title set.

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    std::printf("\033]0;nova@remote: ~\007fake-ssh-connected");
    for (int i = 1; i < argc; ++i)
        std::printf(" %s", argv[i]);
    std::printf("\n\033]2;nova@remote: ~ (osc 2)\033\\remote$ ");
    std::fflush(stdout);
    char line[256];
    while (std::fgets(line, sizeof line, stdin) != nullptr) {
        line[std::strcspn(line, "\r\n")] = 0;
        if (std::strcmp(line, "exit") == 0)
            break;
        std::printf("\033]2;%s\007ran-%s\nremote$ ", line, line);
        std::fflush(stdout);
    }
    std::printf("\033]0;nova@remote: left behind\007Connection to remote closed.\n");
    std::fflush(stdout);
    return 0;
}
