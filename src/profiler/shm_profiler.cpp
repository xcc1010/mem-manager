#include "profiler/shm_profiler.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace platform {
namespace {

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string current_tid() {
    std::ostringstream os;
    os << std::this_thread::get_id();
    return os.str();
}

std::string json_escape(const char* s) {
    std::string out;
    if (!s) {
        return out;
    }
    for (const char* p = s; *p; ++p) {
        switch (*p) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += *p;     break;
        }
    }
    return out;
}

const char* profile_path() {
    const char* p = std::getenv("MEM_PROFILE_PATH");
    return (p && *p) ? p : "shm_profile.jsonl";
}

} // namespace

struct ShmProfiler::Impl {
    struct Live {
        std::string name;
        uint32 size;
        std::int64_t t_map;
    };

    std::mutex mu;
    std::ofstream out;
    std::map<const void*, Live> live;
};

ShmProfiler::ShmProfiler() : impl_(new Impl) {
    impl_->out.open(profile_path(), std::ios::out | std::ios::app);
}

ShmProfiler::~ShmProfiler() {
    delete impl_;
}

ShmProfiler& ShmProfiler::instance() {
    static ShmProfiler inst; // Meyers singleton: thread-safe init since C++11.
    return inst;
}

void ShmProfiler::on_map(const char* op, const char* shmname, const void* addr,
                         uint32 size, INT32 flags, INT32 ret, const char* pgname) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    const std::int64_t t = now_ns();

    if (ret == 0 && addr) {
        impl_->live[addr] = Impl::Live{shmname ? shmname : "", size, t};
    }

    std::ostringstream js;
    js << '{'
       << "\"ts_ns\":" << t
       << ",\"event\":\"" << op << '"'
       << ",\"shmname\":\"" << json_escape(shmname) << '"'
       << ",\"addr\":\"" << addr << '"'
       << ",\"size\":" << size
       << ",\"flags\":" << flags
       << ",\"ret\":" << ret
       << ",\"tid\":\"" << current_tid() << '"';
    if (pgname) {
        js << ",\"pgname\":\"" << json_escape(pgname) << '"';
    }
    js << "}\n";

    impl_->out << js.str();
    impl_->out.flush();
}

void ShmProfiler::on_unmap(const void* addr, INT32 ret) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    const std::int64_t t = now_ns();

    std::ostringstream js;
    js << '{'
       << "\"ts_ns\":" << t
       << ",\"event\":\"unmap\""
       << ",\"addr\":\"" << addr << '"'
       << ",\"ret\":" << ret;

    auto it = impl_->live.find(addr);
    if (it != impl_->live.end()) {
        js << ",\"matched\":true"
           << ",\"shmname\":\"" << json_escape(it->second.name.c_str()) << '"'
           << ",\"size\":" << it->second.size
           << ",\"lifetime_ns\":" << (t - it->second.t_map);
        impl_->live.erase(it);
    } else {
        js << ",\"matched\":false";
    }
    js << ",\"tid\":\"" << current_tid() << '"';
    js << "}\n";

    impl_->out << js.str();
    impl_->out.flush();
}

} // namespace platform
