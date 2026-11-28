#include "signin_forward.h"

#include <string_view>
#include <utility>
#include <vector>

#include "pr/process.h"

namespace nexus {
namespace {

class SshForward final : public SignInForward {
public:
    explicit SshForward(std::function<void()> notify) : wake(std::move(notify)) {}
    ~SshForward() override {
        if (process) prProcessDestroy(process);
        if (launcher) prProcessLauncherDestroy(launcher);
    }

    void start(const std::string& host, std::uint16_t port, bool codexCallback) {
        if (port < 1024 || host.empty() || host.front() == '-') return;
        const auto address = "127.0.0.1:" + std::to_string(port);
        // LocalCommand runs after authentication and successful listener setup. An occupied local
        // port must fail before the browser receives the one-time path. Only the port enters argv.
        std::vector<std::string> args{"-T", "-S", "none", "-o", "ControlMaster=no",
            "-o", "ForkAfterAuthentication=no", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
            "-o", "ConnectTimeout=8", "-o", "GatewayPorts=no", "-o", "ExitOnForwardFailure=yes", "-o", "PermitLocalCommand=yes",
            "-o", "LocalCommand=echo ready", "-o", "StdinNull=no", "-o", "RemoteCommand=none",
            "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=2",
            "-L", address + ":" + address};
        if (codexCallback) {
            // The unmodified Codex CLI owns this callback on the server. Its response also serves
            // the browser's final success page. Bind both localhost families before opening Safari.
            for (const auto* loopback : {"127.0.0.1", "[::1]"}) {
                args.emplace_back("-L");
                args.push_back(std::string(loopback) + ":1455:127.0.0.1:1455");
            }
        }
        args.insert(args.end(), {"--", host, "exec cat >/dev/null"});
        // The SSH session ends on stdin EOF, including when the client process disappears.
        std::vector<PrStringView> words;
        for (const auto& arg : args) words.push_back({arg.data(), arg.size()});
        PrProcessDesc desc{};
        desc.program = {"/usr/bin/ssh", 12};
        desc.arguments = words.data();
        desc.argumentCount = static_cast<uint32_t>(words.size());
        desc.streams = PrProcessStreams_Pipes;
        desc.errors = PrProcessErrors_Discard;
        desc.wakeContext = this;
        desc.wake = [](void* context) {
            auto& callback = static_cast<SshForward*>(context)->wake;
            if (callback) callback();
        };
        launcher = prProcessLauncherCreate(PrProcessBackend_Default);
        if (launcher) prProcessLaunch(launcher, &desc, &process);
    }

    State poll() override {
        if (!process || prProcessState(process).exited) return State::Failed;
        if (ack != "ready\n") {
            uint8_t bytes[8]{};
            const auto count = prProcessRead(process, PrProcessStream_Output, bytes, sizeof(bytes));
            if (count > 0) ack.append(reinterpret_cast<const char*>(bytes), count);
            if (!std::string_view("ready\n").starts_with(ack)) return State::Failed;
        }
        return ack == "ready\n" ? State::Ready : State::Starting;
    }

private:
    std::function<void()> wake;
    PrProcessLauncher* launcher = nullptr;
    PrProcess* process = nullptr;
    std::string ack;
};

} // namespace

std::unique_ptr<SignInForward> forwardSignInPage(const std::string& host, std::uint16_t port, bool codexCallback,
                                                std::function<void()> wake) {
    auto forward = std::make_unique<SshForward>(std::move(wake));
    forward->start(host, port, codexCallback);
    return forward;
}
} // namespace nexus
