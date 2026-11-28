#pragma once

#include "client_module.h"
#include <memory>
#include <functional>

namespace nexus {
// Code is prepared off the UI thread. A frame uses one table throughout its input and drawing.
class ClientReload {
public:
    ClientReload();
    ~ClientReload();
    void start(std::function<void()> wake);
    void stop();
    const ClientModule* current() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
