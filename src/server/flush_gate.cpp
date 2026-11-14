// The flush gate (server/flush_gate.h): a test seam in front of the store's flushes.

#include "server/flush_gate.h"

#include <csignal>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace nexus::server {

namespace {

// What the gate file says: "" when there is none.
std::string gateSays(const std::string& gate) {
    std::ifstream in(gate);
    if (!in) {
        return "";
    }
    std::ostringstream all;
    all << in.rdbuf();
    std::string text = all.str();
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
        text.pop_back();
    }
    return text.empty() ? "hold" : text;
}

class GateFile : public store::AppendFile {
public:
    GateFile(std::unique_ptr<store::AppendFile> inner, std::string path, std::string gate, std::uint64_t durable)
        : inner_(std::move(inner)), path_(std::move(path)), gate_(std::move(gate)), durable_(durable) {}

    store::Status append(std::span<const std::uint8_t> data) override {
        store::Status s = inner_->append(data);
        if (s.is_ok()) {
            written_ += data.size();
        }
        return s;
    }

    store::Status sync() override {
        std::string says = gateSays(gate_);
        if (says == "hold") {
            std::ofstream(gate_ + ".reached") << "reached\n";
            while (says == "hold") {
                ::usleep(10 * 1000);
                says = gateSays(gate_);
            }
        }
        if (says == "lose") {
            // The machine stops between the write and the flush: the written bytes go with it.
            if (::truncate(path_.c_str(), static_cast<off_t>(durable_)) != 0) {
                std::_Exit(3);
            }
            std::raise(SIGKILL);
        }
        store::Status s = inner_->sync();
        if (s.is_ok()) {
            durable_ += written_;
            written_ = 0;
        }
        return s;
    }

private:
    std::unique_ptr<store::AppendFile> inner_;
    std::string path_;
    std::string gate_;
    std::uint64_t durable_;     // the file's bytes its last flush made durable
    std::uint64_t written_ = 0; // written since
};

class GateFs : public store::Fs {
public:
    GateFs(std::unique_ptr<store::Fs> inner, std::string gate) : inner_(std::move(inner)), gate_(std::move(gate)) {}

    store::Status open_append(const std::string& path, std::unique_ptr<store::AppendFile>& out) override {
        std::uint64_t size = 0;
        store::Status s = inner_->size(path, size);
        if (!s.is_ok()) {
            return s;
        }
        std::unique_ptr<store::AppendFile> file;
        s = inner_->open_append(path, file);
        out.reset();
        if (s.is_ok()) {
            out = std::make_unique<GateFile>(std::move(file), path, gate_, size);
        }
        return s;
    }
    store::Status make_dir(const std::string& d) override { return inner_->make_dir(d); }
    store::Status list(const std::string& d, std::vector<std::string>& n) override { return inner_->list(d, n); }
    store::Status size(const std::string& p, std::uint64_t& o) override { return inner_->size(p, o); }
    store::Status read(const std::string& p, std::uint64_t off, std::uint64_t n, store::Bytes& o) override {
        return inner_->read(p, off, n, o);
    }
    store::Status create(const std::string& p) override { return inner_->create(p); }
    store::Status append(const std::string& p, std::span<const std::uint8_t> d) override {
        return inner_->append(p, d);
    }
    store::Status truncate(const std::string& p, std::uint64_t n) override { return inner_->truncate(p, n); }
    store::Status sync(const std::string& p) override { return inner_->sync(p); }
    store::Status sync_dir(const std::string& d) override { return inner_->sync_dir(d); }
    store::Status rename(const std::string& f, const std::string& t) override { return inner_->rename(f, t); }
    store::Status remove(const std::string& p) override { return inner_->remove(p); }
    store::Status write_new(const std::string& p, std::span<const std::uint8_t> d) override {
        return inner_->write_new(p, d);
    }
    store::Status private_dir(const std::string& d) override { return inner_->private_dir(d); }

private:
    std::unique_ptr<store::Fs> inner_;
    std::string gate_;
};

} // namespace

std::unique_ptr<store::Fs> withFlushGate(std::unique_ptr<store::Fs> fs, const std::string& gate) {
    return std::make_unique<GateFs>(std::move(fs), gate);
}

} // namespace nexus::server
