// Per-env V8 CPU profiles for NAPI_V8_CPU_PROFILE_DIR (v8/README.md,
// "Diagnostics").
//
// When the host process sets the variable, unofficial_napi_create_env starts
// a CPU profile for the env and unofficial_napi_release_env stops it, so the
// profile covers everything the env ran in between. It is written to that
// directory as <name>.cpuprofile, in the format Chrome DevTools loads, next to
// <name>.metadata.json, a one-line JSON record of how the session went.
// <name> is env-<pid>-<token>-<ordinal>. The token is random per process, so
// processes that share a directory (or a pid, in different pid namespaces)
// don't pick the same names, and both files are created with O_EXCL, so an
// existing file is never overwritten.
//
// Only the host process's environment can turn this on or choose the path:
// no N-API call, guest import or script can. It runs no JavaScript and makes
// no N-API values. When there is no record file to write to, the record goes
// to stderr, on one line that starts with "NAPI_V8_CPU_PROFILE ".
#ifndef NAPI_V8_ENV_CPU_PROFILE_H_
#define NAPI_V8_ENV_CPU_PROFILE_H_

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <locale>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <v8.h>
#include <v8-profiler.h>

#include "unofficial_napi.h"

#if defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#define NAPI_V8_ENV_CPU_PROFILE_POSIX 1
#endif

namespace v8impl::detail::env_cpu_profile {

constexpr char kDirectoryVariable[] = "NAPI_V8_CPU_PROFILE_DIR";
constexpr char kStderrPrefix[] = "NAPI_V8_CPU_PROFILE ";

// Bounds on what one process writes. Sessions are counted per process from
// the first env created with the variable set, including sessions that failed
// to start. A profile that reaches kMaxSamples is written but marked invalid.
constexpr unsigned kMaxSessions = 8;
constexpr unsigned kMaxSamples = 1 << 20;
constexpr size_t kMaxBytes = 256 * 1024 * 1024;
constexpr unsigned kMaxNodes = 1 << 20;

inline uint64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 16 hex digits, fixed for the life of the process.
inline const std::string& ProcessToken() {
  static const std::string token = [] {
    uint64_t value = static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
    try {
      std::random_device random_device;
      value ^= (static_cast<uint64_t>(random_device()) << 32) ^
               static_cast<uint64_t>(random_device());
    } catch (...) {
      // The wall clock alone still separates processes in practice, and
      // O_EXCL refuses a name that is already taken.
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(value));
    return std::string(hex);
  }();
  return token;
}

class Stream final : public v8::OutputStream {
 public:
  WriteResult WriteAsciiChunk(char* data, int size) override {
    if (size < 0 || static_cast<size_t>(size) > kMaxBytes - bytes.size()) {
      truncated = true;
      return kAbort;
    }
    if (size > 0) bytes.append(data, static_cast<size_t>(size));
    return kContinue;
  }
  void EndOfStream() override { complete = true; }

  bool truncated = false;
  bool complete = false;
  std::string bytes;
};

// V8 13.6's CpuProfile::Serialize writes function names and URLs without JSON
// escaping and narrows absolute timestamps to 32 bits. So the profile is
// exported from the public CpuProfile and CpuProfileNode getters instead, as
// ASCII-only JSON.
class JSONWriter {
 public:
  JSONWriter(Stream& stream, v8::Isolate* isolate) : stream_(stream), isolate_(isolate) {}

  bool good() const { return !stream_.truncated && error == nullptr; }

  void text(const char* value) { append(value, std::strlen(value)); }

  void number(int64_t value) {
    constexpr int64_t kMaxSafeInteger = 9007199254740991LL;
    if (value < -kMaxSafeInteger || value > kMaxSafeInteger) {
      error = "unsafe_json_number";
      return;
    }
    const std::string digits = std::to_string(value);
    append(digits.data(), digits.size());
  }

  void string(v8::Local<v8::String> value) {
    if (value.IsEmpty()) {
      error = "missing_profile_string";
      return;
    }
    const int length = value->Length();
    if (length < 0 || static_cast<size_t>(length) > kMaxBytes) {
      stream_.truncated = true;
      return;
    }
    text("\"");
    uint16_t units[256];
    for (int offset = 0; offset < length && good();) {
      const int count = std::min(256, length - offset);
      value->WriteV2(isolate_, static_cast<uint32_t>(offset), static_cast<uint32_t>(count),
                     units);
      for (int i = 0; i < count && good(); ++i) unit(units[i]);
      offset += count;
    }
    text("\"");
  }

  // Bailout reasons are V8's own ASCII strings. A byte outside ASCII is an
  // error rather than a guess at its encoding.
  void ascii_string(const char* value) {
    text("\"");
    for (size_t i = 0; value[i] != '\0' && good(); ++i) {
      const auto byte = static_cast<unsigned char>(value[i]);
      if (byte >= 128) {
        error = "non_ascii_bailout_reason";
        break;
      }
      unit(byte);
    }
    text("\"");
  }

  const char* error = nullptr;

 private:
  void append(const char* data, size_t size) {
    if (!good()) return;
    if (size > kMaxBytes) {
      stream_.truncated = true;
      return;
    }
    stream_.WriteAsciiChunk(const_cast<char*>(data), static_cast<int>(size));
  }

  // Escapes every UTF-16 unit outside printable ASCII as \uXXXX, lone
  // surrogates and NUL included, so the exact units survive.
  void unit(uint16_t value) {
    if (value == '"' || value == '\\') {
      const char escaped[2] = {'\\', static_cast<char>(value)};
      append(escaped, 2);
    } else if (value >= 32 && value <= 126) {
      const char ascii = static_cast<char>(value);
      append(&ascii, 1);
    } else {
      constexpr char kHex[] = "0123456789abcdef";
      const char escaped[6] = {'\\', 'u', kHex[(value >> 12) & 15], kHex[(value >> 8) & 15],
                               kHex[(value >> 4) & 15], kHex[value & 15]};
      append(escaped, 6);
    }
  }

  Stream& stream_;
  v8::Isolate* isolate_;
};

// Writes the profile to `stream` as a DevTools Profile object: nodes,
// startTime, endTime, samples and timeDeltas. Returns nullptr on success, or
// the reason the profile was refused. Must run on the isolate's thread.
inline const char* ExportProfile(const v8::CpuProfile* profile,
                                 v8::Isolate* isolate,
                                 Stream& stream,
                                 unsigned* node_count) {
  JSONWriter out(stream, isolate);
  std::vector<const v8::CpuProfileNode*> pending{profile->GetTopDownRoot()};
  std::unordered_set<unsigned> ids;
  out.text("{\"nodes\":[");
  bool first = true;
  while (!pending.empty() && out.good()) {
    if (ids.size() >= kMaxNodes) {
      out.error = "tree_node_limit_reached";
      break;
    }
    const v8::CpuProfileNode* node = pending.back();
    pending.pop_back();
    if (node == nullptr || node->GetNodeId() == 0 || !ids.insert(node->GetNodeId()).second) {
      out.error = "invalid_profile_tree";
      break;
    }
    // The getters below make local strings. Without a scope per node they
    // would pile up in the env's root handle scope for the whole tree.
    v8::HandleScope handles(isolate);
    if (!first) out.text(",");
    first = false;
    out.text("{\"id\":");
    out.number(node->GetNodeId());
    out.text(",\"hitCount\":");
    out.number(node->GetHitCount());
    out.text(",\"sourceType\":");
    out.number(node->GetSourceType());
    out.text(",\"callFrame\":{\"functionName\":");
    out.string(node->GetFunctionName());
    // CpuProfileNode positions are 1-based with 0 for unknown; DevTools'
    // are 0-based with -1 for unknown.
    out.text(",\"lineNumber\":");
    out.number(static_cast<int64_t>(node->GetLineNumber()) - 1);
    out.text(",\"columnNumber\":");
    out.number(static_cast<int64_t>(node->GetColumnNumber()) - 1);
    out.text(",\"scriptId\":");
    out.number(node->GetScriptId());
    out.text(",\"url\":");
    out.string(node->GetScriptResourceName());
    out.text("}");
    const int children = node->GetChildrenCount();
    if (children < 0 ||
        static_cast<size_t>(children) > kMaxNodes - ids.size() - pending.size()) {
      out.error = "tree_node_limit_reached";
      break;
    }
    if (children > 0) {
      out.text(",\"children\":[");
      for (int i = 0; i < children && out.good(); ++i) {
        const v8::CpuProfileNode* child = node->GetChild(i);
        if (child == nullptr) {
          out.error = "invalid_profile_tree";
          break;
        }
        if (i > 0) out.text(",");
        out.number(child->GetNodeId());
        pending.push_back(child);
      }
      out.text("]");
    }
    const char* reason = node->GetBailoutReason();
    if (reason != nullptr && reason[0] != '\0') {
      out.text(",\"deoptReason\":");
      out.ascii_string(reason);
    }
    const unsigned line_ticks = node->GetHitLineCount();
    if (line_ticks > kMaxNodes) {
      out.error = "line_tick_limit_reached";
      break;
    }
    if (line_ticks > 0) {
      std::vector<v8::CpuProfileNode::LineTick> entries(line_ticks);
      if (!node->GetLineTicks(entries.data(), line_ticks)) {
        out.error = "line_tick_read_failed";
        break;
      }
      out.text(",\"positionTicks\":[");
      for (unsigned i = 0; i < line_ticks && out.good(); ++i) {
        if (i > 0) out.text(",");
        out.text("{\"line\":");
        out.number(entries[i].line);
        out.text(",\"ticks\":");
        out.number(entries[i].hit_count);
        out.text("}");
      }
      out.text("]");
    }
    out.text("}");
  }
  *node_count = static_cast<unsigned>(ids.size());
  const int64_t start = profile->GetStartTime();
  const int64_t end = profile->GetEndTime();
  out.text("],\"startTime\":");
  out.number(start);
  out.text(",\"endTime\":");
  out.number(end);
  // V8 doesn't keep samples in time order: the ones it takes on the env's
  // thread, when another profile starts and in CollectSample, come through a
  // queue of their own. DevTools wants time order, so they are sorted, stably.
  const int samples = profile->GetSamplesCount();
  std::vector<std::pair<int64_t, unsigned>> ordered;  // Timestamp, node id.
  if (samples < 0 || static_cast<unsigned>(samples) > kMaxSamples) {
    out.error = "invalid_sample_count";
  } else {
    ordered.reserve(static_cast<size_t>(samples));
  }
  for (int i = 0; i < samples && out.good(); ++i) {
    const v8::CpuProfileNode* node = profile->GetSample(i);
    if (node == nullptr || !ids.contains(node->GetNodeId())) {
      out.error = "invalid_sample_node";
      break;
    }
    ordered.emplace_back(profile->GetSampleTimestamp(i), node->GetNodeId());
  }
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  if (out.good() && !ordered.empty() &&
      (start < 0 || ordered.front().first < start || ordered.back().first > end)) {
    out.error = "sample_outside_profile";
  }
  out.text(",\"samples\":[");
  for (size_t i = 0; i < ordered.size() && out.good(); ++i) {
    if (i > 0) out.text(",");
    out.number(ordered[i].second);
  }
  out.text("],\"timeDeltas\":[");
  int64_t previous = start;
  for (size_t i = 0; i < ordered.size() && out.good(); ++i) {
    if (i > 0) out.text(",");
    out.number(ordered[i].first - previous);
    previous = ordered[i].first;
  }
  out.text("]}");
  if (out.good()) stream.EndOfStream();
  return out.error;
}

// One env's session. Owned by the env's UnofficialEnvScope from the end of
// unofficial_napi_create_env until unofficial_napi_release_env finishes it.
struct State {
  std::thread::id owner = std::this_thread::get_id();
  uint64_t created_ns = NowNs();
  uint64_t start_begin_ns = 0;
  uint64_t start_end_ns = 0;
  uint64_t stop_begin_ns = 0;
  uint64_t stop_end_ns = 0;
  int64_t profile_start_us = 0;
  int64_t profile_end_us = 0;
  unsigned ordinal = 0;
  int profile_fd = -1;
  int metadata_fd = -1;
  int samples = 0;
  int io_errno = 0;
  unsigned nodes = 0;
  size_t output_bytes = 0;
  bool sample_limit = false;
  bool byte_limit = false;
  bool valid = false;
  // Also listed in the env's active profiles, which own it: if the session
  // can't be stopped here, env teardown stops and frees it as it does for any
  // session left running.
  unofficial_napi_profile session = nullptr;
  const char* status = "not_started";
  const char* cleanup = "no_session";

  State() = default;
  State(const State&) = delete;
  State& operator=(const State&) = delete;

  ~State() {
#if NAPI_V8_ENV_CPU_PROFILE_POSIX
    if (profile_fd >= 0) ::close(profile_fd);
    if (metadata_fd >= 0) ::close(metadata_fd);
#endif
  }
};

#if NAPI_V8_ENV_CPU_PROFILE_POSIX
inline bool WriteAll(int fd, const std::string& bytes, int* error) {
  size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      *error = count < 0 ? errno : EIO;
      return false;
    }
    offset += static_cast<size_t>(count);
  }
  return true;
}

// Creates both output files in `directory`, which must be an absolute path to
// an existing directory (symbolic links on the way are followed). The files
// themselves are never reached through a link. On failure the status says
// which step failed and io_errno says why.
inline bool Open(State& state, const char* directory) {
  if (directory[0] != '/') {
    state.status = "directory_not_absolute";
    return false;
  }
  const int dir = ::open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir < 0) {
    state.io_errno = errno;
    state.status = "directory_open_failed";
    return false;
  }
  const std::string base = "env-" + std::to_string(::getpid()) + "-" + ProcessToken() + "-" +
                           std::to_string(state.ordinal);
  constexpr int kFlags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC;
  state.metadata_fd = ::openat(dir, (base + ".metadata.json").c_str(), kFlags, 0600);
  if (state.metadata_fd >= 0) {
    state.profile_fd = ::openat(dir, (base + ".cpuprofile").c_str(), kFlags, 0600);
  }
  if (state.metadata_fd < 0 || state.profile_fd < 0) {
    state.io_errno = errno;
    state.status = "output_open_failed";
  }
  ::close(dir);
  return state.metadata_fd >= 0 && state.profile_fd >= 0;
}
#endif

// Writes the session's record to its metadata file, or to stderr when there
// is none or writing it fails.
inline void Record(State& state) {
  auto json = [&state] {
    std::ostringstream out;
    // The host's C++ locale could group digits ("1,048,576").
    out.imbue(std::locale::classic());
    out << "{\"schemaVersion\":1"
        << ",\"ordinal\":" << state.ordinal
        << ",\"status\":\"" << state.status << "\""
        << ",\"valid\":" << (state.valid ? "true" : "false")
        << ",\"cleanup\":\"" << state.cleanup << "\""
        << ",\"errno\":" << state.io_errno
        << ",\"samples\":" << state.samples
        << ",\"nodes\":" << state.nodes
        << ",\"outputBytes\":" << state.output_bytes
        << ",\"sampleLimitReached\":" << (state.sample_limit ? "true" : "false")
        << ",\"byteLimitReached\":" << (state.byte_limit ? "true" : "false")
        << ",\"maxSessions\":" << kMaxSessions
        << ",\"maxSamples\":" << kMaxSamples
        << ",\"maxBytes\":" << kMaxBytes
        << ",\"maxNodes\":" << kMaxNodes
        // 64-bit clock readings are strings: JSON numbers lose precision
        // past 2^53 in most readers.
        << ",\"profileStartUs\":\"" << state.profile_start_us << "\""
        << ",\"profileEndUs\":\"" << state.profile_end_us << "\""
        << ",\"createdNs\":\"" << state.created_ns << "\""
        << ",\"startCallBeginNs\":\"" << state.start_begin_ns << "\""
        << ",\"startCallEndNs\":\"" << state.start_end_ns << "\""
        << ",\"stopCallBeginNs\":\"" << state.stop_begin_ns << "\""
        << ",\"stopCallEndNs\":\"" << state.stop_end_ns << "\""
        << "}\n";
    return out.str();
  };
#if NAPI_V8_ENV_CPU_PROFILE_POSIX
  if (state.metadata_fd >= 0) {
    if (WriteAll(state.metadata_fd, json(), &state.io_errno)) return;
    state.valid = false;
    state.status = "metadata_write_failed";
  }
#endif
  std::fprintf(stderr, "%s%s", kStderrPrefix, json().c_str());
  std::fflush(stderr);
}

}  // namespace v8impl::detail::env_cpu_profile

#endif  // NAPI_V8_ENV_CPU_PROFILE_H_
