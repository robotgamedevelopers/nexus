#include "server/driver_table.h"

namespace nexus::server {

namespace {

std::vector<std::string> words(std::string_view line) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < line.size()) {
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) {
            ++at;
        }
        std::size_t end = at;
        while (end < line.size() && line[end] != ' ' && line[end] != '\t') {
            ++end;
        }
        if (end > at) {
            out.emplace_back(line.substr(at, end - at));
        }
        at = end;
    }
    return out;
}

} // namespace

DriverTable parseDriverTable(std::string_view text, const std::string& home) {
    DriverTable table;
    std::size_t number = 0;
    while (!text.empty()) {
        ++number;
        const std::size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view() : text.substr(newline + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        std::vector<std::string> w = words(line);
        if (w.empty() || w[0][0] == '#') {
            continue;
        }
        const std::string where = "line " + std::to_string(number) + ": ";
        if (w[0] == "driver" && w.size() >= 3) {
            table.drivers[w[1]] = std::vector<std::string>(w.begin() + 2, w.end());
        } else if (w[0] == "friend" && (w.size() == 3 || w.size() == 4)) {
            std::string cwd = w.size() == 4 ? w[3] : home;
            if (cwd == "~" || cwd.rfind("~/", 0) == 0) {
                cwd = home + cwd.substr(1);
            }
            table.friends[w[1]] = DriverTable::Friend{w[2], cwd};
        } else if (w[0] == "driver") {
            table.errors.push_back(where + "a driver line is `driver <harness> <program> [<argument>...]`");
        } else if (w[0] == "friend") {
            table.errors.push_back(where + "a friend line is `friend <name> <harness> [<directory>]`");
        } else {
            table.errors.push_back(where + "\"" + w[0] + "\" is not `driver` or `friend`");
        }
    }
    return table;
}

ResolvedDriver resolveDriver(const DriverTable& table, const std::string& name, const std::string& path) {
    ResolvedDriver r;
    std::string errors;
    for (const std::string& e : table.errors) {
        errors += " " + path + " " + e + ".";
    }
    auto f = table.friends.find(name);
    if (f == table.friends.end()) {
        r.problem = name + " has no driver, so the prompt was not sent. Name " + name + "'s harness with a line "
            "`friend " + name + " <harness> [<directory>]` in " + path + ", and its driver with a line `driver "
            "<harness> <program> [<argument>...]`; the file is read at each prompt." + errors;
        return r;
    }
    auto d = table.drivers.find(f->second.harness);
    if (d == table.drivers.end()) {
        r.problem = name + "'s harness, " + f->second.harness + ", has no driver, so the prompt was not sent. Add a "
            "line `driver " + f->second.harness + " <program> [<argument>...]` to " + path + "; the file is read at "
            "each prompt." + errors;
        return r;
    }
    r.command = d->second;
    for (std::size_t i = 1; i < r.command.size(); ++i) {
        if (r.command[i] == "{friend}") r.command[i] = name;
        else if (r.command[i] == "{harness}") r.command[i] = f->second.harness;
    }
    r.cwd = f->second.cwd;
    return r;
}

} // namespace nexus::server

namespace nexus::server {

accounts::Registry accountsFromDrivers(const DriverTable& table, const std::string& machine) {
    accounts::Registry r;
    std::map<std::string, std::string> accountOf;   // harness: the account its line passes
    for (const auto& [harness, command] : table.drivers) {
        for (std::size_t i = 0; i + 1 < command.size(); ++i) {
            if (command[i] == "--account" && accounts::isName(command[i + 1]) && accounts::isProvider(harness)) {
                accountOf[harness] = command[i + 1];
                if (r.account(command[i + 1]) == nullptr) {
                    r.accounts.push_back(accounts::Account{command[i + 1], harness});
                }
            }
        }
    }
    for (const auto& [name, f] : table.friends) {
        if (!accounts::isName(name)) {
            continue;
        }
        accounts::Friend fr{name, {}, {}};
        fr.limit = false;   // it ran unlimited before the field: nothing changes under the person (docs/accounts.md)
        auto a = accountOf.find(f.harness);
        if (a != accountOf.end() && r.account(a->second)->provider == f.harness) {
            fr.accounts = {a->second};
            fr.preferred = a->second;
        }
        if (accounts::isName(machine) && accounts::isMachine(machine) && accounts::isDir(f.cwd)) {
            fr.instances.push_back(accounts::Instance{machine, machine, f.cwd});
        }
        if (!fr.accounts.empty() || !fr.instances.empty()) {
            r.friends.push_back(std::move(fr));
        }
    }
    return r;
}

} // namespace nexus::server
