// Client preparation of an explicit login. Every account mutation is requested from and confirmed by the server.
// Browser logout is confirmed by the person; opening a page is not evidence of logout. models/LoginPreparation.tla.
#pragma once

namespace nexus {
class LoginPreparation {
public:
    enum class Phase { Waiting, SigningOut, Selecting, Browser, Ready, Failed };
    struct Input {
        bool valid = true;
        bool busy = false;
        bool acknowledged = false;
        bool signedOut = false; // this request's durable success notice AND the latest unsigned account state
        bool selected = false;
        bool confirmed = false;
    };
    explicit LoginPreparation(bool browser) : browser_(browser) {}
    Phase phase() const { return phase_; }
    Phase step(Input in) {
        if (!in.valid) phase_ = Phase::Failed;
        else switch (phase_) {
            case Phase::Waiting:
                if (!in.busy) phase_ = Phase::SigningOut;
                break;
            case Phase::SigningOut:
                if (in.acknowledged) phase_ = in.signedOut ? Phase::Selecting : Phase::Failed;
                break;
            case Phase::Selecting:
                if (in.acknowledged) phase_ = in.selected && in.signedOut ?
                    (browser_ ? Phase::Browser : Phase::Ready) : Phase::Failed;
                break;
            case Phase::Browser:
                if (in.confirmed) phase_ = in.selected && in.signedOut ? Phase::Ready : Phase::Failed;
                break;
            case Phase::Ready: case Phase::Failed: break;
        }
        return phase_;
    }
    static const char* name(Phase p) {
        switch (p) {
            case Phase::Waiting: return "waiting";
            case Phase::SigningOut: return "signout";
            case Phase::Selecting: return "select";
            case Phase::Browser: return "browser";
            case Phase::Ready: return "ready";
            case Phase::Failed: return "failed";
        }
        return "failed";
    }
private:
    Phase phase_ = Phase::Waiting;
    bool browser_;
};
} // namespace nexus
