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

#ifdef _WIN32
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace platform {
namespace {

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int current_pid() {
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
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

// Each process (the CP platform process and every DP process) writes its own
// file, so concurrent processes never interleave into one log. MEM_PROFILE_PATH
// overrides the default; a literal "%p" in it is replaced by the pid.
std::string profile_path(int pid) {
    const char* env = std::getenv("MEM_PROFILE_PATH");
    std::string path = (env && *env) ? std::string(env)
                                      : ("shm_profile." + std::to_string(pid) + ".jsonl");
    const std::string token = "%p";
    const auto pos = path.find(token);
    if (pos != std::string::npos) {
        path.replace(pos, token.size(), std::to_string(pid));
    }
    return path;
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
    int pid = 0;

    std::map<const void*, Live> live;

    // High-water marks: peak number of simultaneously-live mappings and peak
    // simultaneously-live bytes. These size the Step-2 pool far better than
    // cumulative totals do.
    std::uint64_t live_bytes = 0;
    std::uint64_t peak_count = 0;
    std::uint64_t peak_bytes = 0;

    // Lifetime tallies for the exit summary.
    std::uint64_t total_map_calls = 0;    // map + map_on_pg events
    std::uint64_t total_map_failures = 0; // ret != 0
    std::uint64_t total_unmaps = 0;
    std::uint64_t unmatched_unmaps = 0;

    // Fields appended to every event so a reader can reconstruct the live-set
    // time series and confirm peaks without replaying the whole log.
    void append_common(std::ostringstream& js) {
        js << ",\"pid\":" << pid
           << ",\"live_count\":" << live.size()
           << ",\"live_bytes\":" << live_bytes
           << ",\"tid\":\"" << current_tid() << '"';
    }
};

ShmProfiler::ShmProfiler() : impl_(new Impl) {
    impl_->pid = current_pid();
    impl_->out.open(profile_path(impl_->pid), std::ios::out | std::ios::app);
}

ShmProfiler::~ShmProfiler() {
    // At process exit, dump anything still mapped (long-lived / leaked) and a
    // one-line summary. Runs single-threaded during static destruction.
    const std::int64_t t = now_ns();
    for (const auto& [addr, e] : impl_->live) {
        std::ostringstream js;
        js << '{'
           << "\"ts_ns\":" << t
           << ",\"event\":\"live_at_exit\""
           << ",\"shmname\":\"" << json_escape(e.name.c_str()) << '"'
           << ",\"addr\":\"" << addr << '"'
           << ",\"size\":" << e.size
           << ",\"age_ns\":" << (t - e.t_map)
           << ",\"pid\":" << impl_->pid
           << "}\n";
        impl_->out << js.str();
    }

    std::ostringstream js;
    js << '{'
       << "\"ts_ns\":" << t
       << ",\"event\":\"summary\""
       << ",\"pid\":" << impl_->pid
       << ",\"peak_live_count\":" << impl_->peak_count
       << ",\"peak_live_bytes\":" << impl_->peak_bytes
       << ",\"total_maps\":" << impl_->total_map_calls
       << ",\"total_map_failures\":" << impl_->total_map_failures
       << ",\"total_unmaps\":" << impl_->total_unmaps
       << ",\"unmatched_unmaps\":" << impl_->unmatched_unmaps
       << ",\"still_live_at_exit\":" << impl_->live.size()
       << "}\n";
    impl_->out << js.str();
    impl_->out.flush();

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

    ++impl_->total_map_calls;
    if (ret != 0) {
        ++impl_->total_map_failures;
    }
    if (ret == 0 && addr) {
        impl_->live[addr] = Impl::Live{shmname ? shmname : "", size, t};
        impl_->live_bytes += size;
        if (impl_->live.size() > impl_->peak_count) {
            impl_->peak_count = impl_->live.size();
        }
        if (impl_->live_bytes > impl_->peak_bytes) {
            impl_->peak_bytes = impl_->live_bytes;
        }
    }

    std::ostringstream js;
    js << '{'
       << "\"ts_ns\":" << t
       << ",\"event\":\"" << op << '"'
       << ",\"shmname\":\"" << json_escape(shmname) << '"'
       << ",\"addr\":\"" << addr << '"'
       << ",\"size\":" << size
       << ",\"flags\":" << flags
       << ",\"ret\":" << ret;
    if (pgname) {
        js << ",\"pgname\":\"" << json_escape(pgname) << '"';
    }
    impl_->append_common(js);
    js << "}\n";

    impl_->out << js.str();
    impl_->out.flush();
}

void ShmProfiler::on_unmap(const void* addr, INT32 ret) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    const std::int64_t t = now_ns();

    ++impl_->total_unmaps;

    std::ostringstream js;
    js << '{'
       << "\"ts_ns\":" << t
       << ",\"event\":\"unmap\""
       << ",\"addr\":\"" << addr << '"'
       << ",\"ret\":" << ret;

    auto it = impl_->live.find(addr);
    if (it != impl_->live.end()) {
        impl_->live_bytes -= it->second.size;
        js << ",\"matched\":true"
           << ",\"shmname\":\"" << json_escape(it->second.name.c_str()) << '"'
           << ",\"size\":" << it->second.size
           << ",\"lifetime_ns\":" << (t - it->second.t_map);
        impl_->live.erase(it);
    } else {
        ++impl_->unmatched_unmaps;
        js << ",\"matched\":false";
    }
    impl_->append_common(js);
    js << "}\n";

    impl_->out << js.str();
    impl_->out.flush();
}

} // namespace platform
