// A remote enrollment page has a fixed loopback origin for its cookie and POST checks.
// The desktop keeps that origin by owning the same port locally for the lifetime of the flow.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace nexus {

class SignInForward {
public:
    enum class State { Starting, Ready, Failed };
    virtual ~SignInForward() = default;
    virtual State poll() = 0;
};

std::unique_ptr<SignInForward> forwardSignInPage(const std::string& host, std::uint16_t port, bool codexCallback,
                                                std::function<void()> wake);

} // namespace nexus
