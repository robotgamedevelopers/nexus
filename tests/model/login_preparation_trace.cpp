// Deterministic traces of the production transition function, including refused and delayed server responses.
#include "login_preparation.h"
#include <fstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const std::string fault(argv[1]);
    std::ofstream out(argv[2]);
    if (!out) return 2;
    using Machine = nexus::LoginPreparation;
    const auto bit = [](bool b) { return b ? "TRUE" : "FALSE"; };
    Machine machine(true);
    out << "---- MODULE LoginPreparationTraceData ----\nTrace == <<\n";
    const auto emit = [&](Machine::Input i, Machine::Phase phase, bool first, bool reset = false) {
        out << (first ? "" : ",\n") << "[phase |-> \"" << Machine::name(phase) << "\", input |-> [valid |-> "
            << bit(i.valid) << ", busy |-> " << bit(i.busy) << ", acknowledged |-> " << bit(i.acknowledged)
            << ", signedOut |-> " << bit(i.signedOut) << ", selected |-> " << bit(i.selected)
            << ", confirmed |-> " << bit(i.confirmed) << "], reset |-> " << bit(reset) << "]";
    };
    emit({}, machine.phase(), true);
    std::vector<Machine::Input> steps{
        {.busy = true}, {.busy = true}, {}, {},
        {.acknowledged = true, .signedOut = true}, {},
        {.acknowledged = true, .signedOut = true, .selected = true},
        {.signedOut = true, .selected = true},
        {.signedOut = true, .selected = true, .confirmed = true},
        {.valid = false}, {},
    };
    for (std::size_t n = 0; n < steps.size(); ++n) {
        auto actual = steps[n];
        // Mutations at the host boundary prove early ACK and browser assumptions are rejected by the model.
        if (fault == "ack" && n == 3) { actual.acknowledged = true; actual.signedOut = true; }
        if (fault == "browser" && n == 7) actual.confirmed = true;
        emit(steps[n], machine.step(actual), false);
    }
    // Cover every input at every reachable phase, including refusals, changed selections and link loss.
    for (unsigned phase = 0; phase < 6; ++phase) for (unsigned bits = 0; bits < 64; ++bits) {
        machine = Machine(true);
        emit({}, machine.phase(), false, true);
        const std::vector<Machine::Input> path{{}, {.acknowledged = true, .signedOut = true},
            {.acknowledged = true, .signedOut = true, .selected = true},
            {.signedOut = true, .selected = true, .confirmed = true}, {.valid = false}};
        for (unsigned n = 0; n < phase; ++n) emit(path[n], machine.step(path[n]), false);
        Machine::Input input{bool(bits & 1), bool(bits & 2), bool(bits & 4), bool(bits & 8), bool(bits & 16), bool(bits & 32)};
        emit(input, machine.step(input), false);
    }
    out << "\n>>\n====\n";
    return out ? 0 : 2;
}
