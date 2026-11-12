// NAPI_V8_CPU_PROFILE_DIR: each env records a V8 CPU profile from
// unofficial_napi_create_env to unofficial_napi_release_env, and writes it to
// that directory with a one-line record of how the session went. Directories
// that can't be used are reported on stderr, and the env runs as usual.
//
// V8 provider, Linux and macOS only. The session limit is per process, and
// ctest runs each test in a process of its own. Run by hand without a
// --gtest_filter, the binary skips the tests that no longer fit in it.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <locale>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <v8.h>
#include <v8-profiler.h>

#include "test_env.h"

class Test76EnvCpuProfile : public FixtureTestBase {};

namespace {

namespace fs = std::filesystem;

constexpr char kVariable[] = "NAPI_V8_CPU_PROFILE_DIR";
constexpr char kStderrPrefix[] = "NAPI_V8_CPU_PROFILE ";
constexpr int kMaxSessions = 8;

// Sessions this process has used. As in the provider, every env created with
// the variable set uses one, whether or not its session starts.
int g_sessions_used = 0;
constexpr char kRunAlone[] = "the process has used up its sessions; ctest runs each test alone";

// Takes `count` sessions, or returns false when they don't fit and the test
// must skip.
bool ReserveSessions(int count) {
  if (g_sessions_used + count > kMaxSessions) return false;
  g_sessions_used += count;
  return true;
}

// Sets the variable (or unsets it, for nullptr) and restores it on exit.
class ScopedProfileDir {
 public:
  explicit ScopedProfileDir(const char* value) {
    if (const char* old = std::getenv(kVariable)) saved_ = old;
    if (value != nullptr) {
      setenv(kVariable, value, 1);
    } else {
      unsetenv(kVariable);
    }
  }
  ~ScopedProfileDir() {
    if (saved_) {
      setenv(kVariable, saved_->c_str(), 1);
    } else {
      unsetenv(kVariable);
    }
  }
  ScopedProfileDir(const ScopedProfileDir&) = delete;
  ScopedProfileDir& operator=(const ScopedProfileDir&) = delete;

 private:
  std::optional<std::string> saved_;
};

class TempDir {
 public:
  TempDir() {
    std::string pattern = (fs::temp_directory_path() / "napi-cpu-profile-XXXXXX").string();
    if (mkdtemp(pattern.data()) != nullptr) path_ = pattern;
  }
  ~TempDir() {
    std::error_code ignored;
    fs::permissions(path_, fs::perms::owner_all, ignored);
    fs::remove_all(path_, ignored);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

// Sends stderr to a file until Stop().
class StderrCapture {
 public:
  StderrCapture() {
    std::fflush(stderr);
    std::string pattern = (fs::temp_directory_path() / "napi-cpu-profile-stderr-XXXXXX").string();
    file_ = mkstemp(pattern.data());
    if (file_ < 0) return;
    unlink(pattern.c_str());
    saved_ = dup(STDERR_FILENO);
    dup2(file_, STDERR_FILENO);
  }
  ~StderrCapture() { Stop(); }
  StderrCapture(const StderrCapture&) = delete;
  StderrCapture& operator=(const StderrCapture&) = delete;

  // Restores stderr and returns the lines written with the profile prefix,
  // without it.
  std::vector<std::string> Stop() {
    if (saved_ >= 0) {
      std::fflush(stderr);
      dup2(saved_, STDERR_FILENO);
      close(saved_);
      saved_ = -1;
      std::string text;
      char buffer[4096];
      lseek(file_, 0, SEEK_SET);
      for (ssize_t count; (count = read(file_, buffer, sizeof(buffer))) > 0;) {
        text.append(buffer, static_cast<size_t>(count));
      }
      close(file_);
      file_ = -1;
      std::istringstream stream(text);
      for (std::string line; std::getline(stream, line);) {
        if (line.rfind(kStderrPrefix, 0) == 0) {
          records_.push_back(line.substr(strlen(kStderrPrefix)));
        }
      }
    }
    return records_;
  }

 private:
  int file_ = -1;
  int saved_ = -1;
  std::vector<std::string> records_;
};

std::vector<std::string> ListDir(const fs::path& dir) {
  std::vector<std::string> names;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(dir, error)) {
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

napi_status RunJs(napi_env env, const std::string& source_text, napi_value* result = nullptr) {
  napi_value source = nullptr;
  napi_value ignored = nullptr;
  napi_status status =
      napi_create_string_utf8(env, source_text.data(), source_text.size(), &source);
  if (status != napi_ok) return status;
  return napi_run_script(env, source, result != nullptr ? result : &ignored);
}

int32_t RunJsInt(napi_env env, const std::string& source_text) {
  napi_value result = nullptr;
  int32_t value = -1;
  if (RunJs(env, source_text, &result) != napi_ok ||
      napi_get_value_int32(env, result, &value) != napi_ok) {
    return -1;
  }
  return value;
}

// Takes a sample of the current stack, so the profile is sure to show the
// JavaScript function that calls this.
napi_value CollectSample(napi_env env, napi_callback_info) {
  v8::CpuProfiler::CollectSample(v8::Isolate::GetCurrent());
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

// A method name with characters JSON needs escaped and characters outside
// ASCII: a quote, a backslash, a newline, a Latin-1 letter and a surrogate
// pair. V8 13.6's CpuProfile::Serialize writes names unescaped. (No lone
// surrogate: V8 keeps profile names as UTF-8, which can't hold one.)
constexpr char kNameLiteral[] =
    R"JS('quote" back\\slash new\nline caf\u00e9 \u{1F49C}')JS";

// Defines work(ms), which takes a sample, runs for `ms` (20 by default)
// milliseconds and returns 1, however the thread is scheduled.
void DefineWork(napi_env env) {
  napi_value global = nullptr;
  napi_value function = nullptr;
  ASSERT_EQ(napi_get_global(env, &global), napi_ok);
  ASSERT_EQ(napi_create_function(env, "collectSample", NAPI_AUTO_LENGTH, CollectSample, nullptr,
                                 &function),
            napi_ok);
  ASSERT_EQ(napi_set_named_property(env, global, "collectSample", function), napi_ok);
  ASSERT_EQ(RunJs(env, std::string("globalThis.work = Object.values({ ") + kNameLiteral +
                           "(ms = 20) {"
                           "  collectSample();"
                           "  const end = Date.now() + ms;"
                           "  let n = 0;"
                           "  do n++; while (Date.now() < end);"
                           "  return 1;"
                           "} })[0];"),
            napi_ok);
}

// Runs `script` in a fresh env, with the variable unset, after defining each
// global as a string. Returns the script's string result, or what went wrong.
std::string CheckInJs(const std::string& script,
                      const std::vector<std::pair<const char*, std::string>>& globals) {
  ScopedProfileDir unset(nullptr);
  napi_env env = nullptr;
  unofficial_napi_env_owner owner = nullptr;
  unofficial_napi_env_create_options options{};
  options.size = sizeof(options);
  options.version = UNOFFICIAL_NAPI_ENV_CREATE_OPTIONS_VERSION;
  if (unofficial_napi_create_env(NAPI_TEST_MODULE_API_VERSION, &options, &env, &owner) !=
      napi_ok) {
    return "could not create the checking env";
  }
  std::string outcome;
  napi_value global = nullptr;
  napi_get_global(env, &global);
  for (const auto& [name, text] : globals) {
    napi_value value = nullptr;
    napi_create_string_utf8(env, text.data(), text.size(), &value);
    napi_set_named_property(env, global, name, value);
  }
  napi_value result = nullptr;
  if (RunJs(env, script, &result) != napi_ok) {
    napi_value exception = nullptr;
    napi_value message = nullptr;
    napi_get_and_clear_last_exception(env, &exception);
    napi_coerce_to_string(env, exception, &message);
    result = message;
  }
  char buffer[4096] = {};
  size_t length = 0;
  if (result == nullptr ||
      napi_get_value_string_utf8(env, result, buffer, sizeof(buffer), &length) != napi_ok) {
    outcome = "the check returned no string";
  } else {
    outcome.assign(buffer, length);
  }
  unofficial_napi_release_env(owner, nullptr);
  return outcome;
}

// The output files of one session, as <base>.cpuprofile and
// <base>.metadata.json.
struct Session {
  unsigned ordinal = 0;
  fs::path profile;
  fs::path metadata;
};

// Finds the sessions in `dir`. Fails if anything else is there.
testing::AssertionResult FindSessions(const fs::path& dir, std::vector<Session>* sessions) {
  const std::regex name("env-" + std::to_string(getpid()) +
                        "-[0-9a-f]{16}-([0-9]+)\\.(cpuprofile|metadata\\.json)");
  std::vector<std::string> profiles;
  std::vector<std::string> records;
  for (const std::string& file : ListDir(dir)) {
    std::smatch match;
    if (!std::regex_match(file, match, name)) {
      return testing::AssertionFailure() << "unexpected file " << file;
    }
    (match[2] == "cpuprofile" ? profiles : records).push_back(file);
  }
  if (profiles.size() != records.size()) {
    return testing::AssertionFailure()
           << profiles.size() << " profiles but " << records.size() << " records";
  }
  for (const std::string& file : profiles) {
    std::smatch match;
    std::regex_match(file, match, name);
    const std::string base = file.substr(0, file.size() - strlen(".cpuprofile"));
    Session session;
    session.ordinal = static_cast<unsigned>(std::stoul(match[1]));
    session.profile = dir / file;
    session.metadata = dir / (base + ".metadata.json");
    if (!fs::exists(session.metadata)) {
      return testing::AssertionFailure() << "no record next to " << file;
    }
    sessions->push_back(session);
  }
  std::sort(sessions->begin(), sessions->end(),
            [](const Session& a, const Session& b) { return a.ordinal < b.ordinal; });
  return testing::AssertionSuccess();
}

// Checks a written session: a profile in DevTools' format, which matches its
// record and shows work(). `expectedStatus` is complete, or
// sample_limit_reached for a profile that is written but not valid.
const std::string kCheckWrittenProfile = std::string(R"JS((() => {
  const fail = (message) => { throw new Error(message); };
  const expectedName = )JS") + kNameLiteral + R"JS(;
  if (!recordText.endsWith('\n') || recordText.indexOf('\n') !== recordText.length - 1) {
    fail('the record is not one line');
  }
  const record = JSON.parse(recordText);
  const capped = expectedStatus === 'sample_limit_reached';
  if (record.schemaVersion !== 1 || record.status !== expectedStatus ||
      record.valid !== !capped || record.cleanup !== 'stopped' || record.errno !== 0 ||
      record.sampleLimitReached !== capped || record.byteLimitReached !== false) {
    fail('record: ' + recordText);
  }
  const profile = JSON.parse(profileText);
  if (!/^[\x20-\x7e]*$/.test(profileText)) fail('the profile is not printable ASCII');
  const keys = Object.keys(profile).sort().join();
  if (keys !== 'endTime,nodes,samples,startTime,timeDeltas') fail('profile keys: ' + keys);
  if (record.outputBytes !== profileText.length) fail('outputBytes: ' + record.outputBytes);
  if (record.profileStartUs !== String(profile.startTime) ||
      record.profileEndUs !== String(profile.endTime) || !(profile.endTime >= profile.startTime)) {
    fail('profile times: ' + profile.startTime + ' ' + profile.endTime);
  }
  const { nodes, samples, timeDeltas } = profile;
  if (record.nodes !== nodes.length) fail('record.nodes: ' + record.nodes);
  if (samples.length < 1 || record.samples !== samples.length ||
      timeDeltas.length !== samples.length ||
      (capped ? samples.length !== record.maxSamples : samples.length >= record.maxSamples)) {
    fail('samples: ' + samples.length + ', timeDeltas: ' + timeDeltas.length);
  }
  const ids = new Set();
  for (const node of nodes) {
    if (!Number.isInteger(node.id) || node.id <= 0 || ids.has(node.id)) fail('node id ' + node.id);
    ids.add(node.id);
    const frame = node.callFrame;
    if (typeof frame.functionName !== 'string' || typeof frame.url !== 'string' ||
        !Number.isInteger(frame.lineNumber) || !Number.isInteger(frame.columnNumber) ||
        !Number.isInteger(frame.scriptId) || !Number.isInteger(node.hitCount)) {
      fail('node ' + JSON.stringify(node));
    }
  }
  const children = new Set();
  for (const node of nodes) {
    for (const child of node.children ?? []) {
      if (!ids.has(child) || children.has(child)) fail('child ' + child);
      children.add(child);
    }
  }
  const roots = nodes.filter((node) => !children.has(node.id));
  if (roots.length !== 1 || roots[0].callFrame.functionName !== '(root)') fail('roots');
  for (const sample of samples) if (!ids.has(sample)) fail('sample ' + sample);
  let total = 0;
  for (const delta of timeDeltas) {
    if (!Number.isInteger(delta) || delta < 0) fail('time delta ' + delta);
    total += delta;
  }
  if (total > profile.endTime - profile.startTime) fail('samples after the end');
  if (!nodes.some((node) => node.callFrame.functionName === expectedName)) {
    const names = [...new Set(nodes.map((node) => node.callFrame.functionName))];
    fail('no node for work(), only ' + JSON.stringify(names));
  }
  return 'ok';
})())JS";

std::string CheckWrittenSession(const Session& session, const char* status) {
  return CheckInJs(kCheckWrittenProfile, {{"profileText", ReadFile(session.profile)},
                                          {"recordText", ReadFile(session.metadata)},
                                          {"expectedStatus", status}});
}

std::string CheckCompleteSession(const Session& session) {
  return CheckWrittenSession(session, "complete");
}

// Checks a record of a session that never started. `errors` lists the errno
// values accepted.
std::string CheckRefusedRecord(const std::string& record,
                               const char* status,
                               std::initializer_list<int> errors) {
  std::string accepted;
  for (int error : errors) accepted += (accepted.empty() ? "" : ",") + std::to_string(error);
  return CheckInJs(R"JS((() => {
    const parsed = JSON.parse(record);
    if (parsed.status !== status || !errors.split(',').map(Number).includes(parsed.errno) ||
        parsed.valid !== false ||
        parsed.cleanup !== 'no_session' || parsed.samples !== 0 || parsed.outputBytes !== 0) {
      throw new Error('record: ' + record);
    }
    return 'ok';
  })())JS",
                   {{"record", record}, {"status", status}, {"errors", accepted}});
}

// Groups digits by thousands, as many named locales do.
struct GroupingPunct : std::numpunct<char> {
  char do_thousands_sep() const override { return ','; }
  std::string do_grouping() const override { return "\3"; }
};

// Makes `locale` the global C++ locale, and restores the previous one on exit.
class ScopedGlobalLocale {
 public:
  explicit ScopedGlobalLocale(const std::locale& locale) : saved_(std::locale::global(locale)) {}
  ~ScopedGlobalLocale() { std::locale::global(saved_); }
  ScopedGlobalLocale(const ScopedGlobalLocale&) = delete;
  ScopedGlobalLocale& operator=(const ScopedGlobalLocale&) = delete;

 private:
  std::locale saved_;
};

// Creates an env with the variable set to `value`, checks that it runs JS,
// releases it, and returns what it wrote on stderr.
std::vector<std::string> RunRefusedEnv(NapiTestRuntime* runtime, const std::string& value) {
  ScopedProfileDir set(value.c_str());
  StderrCapture capture;
  {
    EnvScope s(runtime);
    EXPECT_EQ(RunJsInt(s.env, "6 * 7"), 42);
  }
  return capture.Stop();
}

}  // namespace

TEST_F(Test76EnvCpuProfile, WritesTheProfileAndItsRecordWhenTheEnvIsReleased) {
  if (!ReserveSessions(1)) GTEST_SKIP() << kRunAlone;
  TempDir dir;
  ASSERT_FALSE(dir.path().empty());
  std::vector<std::string> records;
  {
    ScopedProfileDir set(dir.path().c_str());
    StderrCapture capture;
    {
      EnvScope s(runtime_.get());
      ASSERT_NE(s.env, nullptr);
      // Both files exist from the start, and stay empty until the release.
      std::vector<Session> sessions;
      ASSERT_TRUE(FindSessions(dir.path(), &sessions));
      ASSERT_EQ(sessions.size(), 1u);
      EXPECT_EQ(fs::file_size(sessions[0].profile), 0u);
      EXPECT_EQ(fs::file_size(sessions[0].metadata), 0u);
      DefineWork(s.env);
      EXPECT_EQ(RunJsInt(s.env, "work()"), 1);
    }
    records = capture.Stop();
  }
  EXPECT_TRUE(records.empty()) << records[0];

  std::vector<Session> sessions;
  ASSERT_TRUE(FindSessions(dir.path(), &sessions));
  ASSERT_EQ(sessions.size(), 1u);
  EXPECT_EQ(CheckCompleteSession(sessions[0]), "ok");
}

TEST_F(Test76EnvCpuProfile, RunsNoJavaScriptAndIgnoresAPendingException) {
  if (!ReserveSessions(1)) GTEST_SKIP() << kRunAlone;
  TempDir dir;
  {
    ScopedProfileDir set(dir.path().c_str());
    EnvScope s(runtime_.get());
    DefineWork(s.env);
    EXPECT_EQ(RunJsInt(s.env, "work()"), 1);
    // Replaced globals would throw if the provider ran JavaScript to write the
    // profile, and the env is released with an exception pending.
    EXPECT_EQ(RunJs(s.env,
                    "for (const name of ['JSON', 'Uint8Array', 'String', 'Array']) {"
                    "  globalThis[name] = function() { throw new Error('ran ' + name); };"
                    "}"
                    "Object.defineProperty(Object.prototype, 'toJSON', {"
                    "  get() { throw new Error('ran toJSON'); }"
                    "});"
                    "throw { sentinel: 1 };"),
              napi_pending_exception);
  }
  std::vector<Session> sessions;
  ASSERT_TRUE(FindSessions(dir.path(), &sessions));
  ASSERT_EQ(sessions.size(), 1u);
  EXPECT_EQ(CheckCompleteSession(sessions[0]), "ok");
}

// The host may set a C++ locale that groups digits. The record's numbers are
// written in the classic one, or the record wouldn't be JSON.
TEST_F(Test76EnvCpuProfile, WritesItsRecordWhateverTheHostLocale) {
  if (!ReserveSessions(1)) GTEST_SKIP() << kRunAlone;
  TempDir dir;
  {
    ScopedProfileDir set(dir.path().c_str());
    ScopedGlobalLocale grouping(std::locale(std::locale::classic(), new GroupingPunct));
    EnvScope s(runtime_.get());
    DefineWork(s.env);
    EXPECT_EQ(RunJsInt(s.env, "work()"), 1);
  }
  std::vector<Session> sessions;
  ASSERT_TRUE(FindSessions(dir.path(), &sessions));
  ASSERT_EQ(sessions.size(), 1u);
  EXPECT_EQ(CheckCompleteSession(sessions[0]), "ok");
}

// The env's session shares the env's CpuProfiler with
// unofficial_napi_profile_start's. Each start also makes V8 take a sample on
// the env's thread, out of time order, which the profile must still hold in
// order.
TEST_F(Test76EnvCpuProfile, KeepsItsSessionApartFromProfileStartAndStop) {
  if (!ReserveSessions(1)) GTEST_SKIP() << kRunAlone;
  TempDir dir;
  {
    ScopedProfileDir set(dir.path().c_str());
    EnvScope s(runtime_.get());
    DefineWork(s.env);
    unofficial_napi_profile_start_result started = unofficial_napi_profile_start_busy;
    for (int round = 0; round < 64; ++round) {
      unofficial_napi_profile profile = nullptr;
      ASSERT_EQ(unofficial_napi_profile_start(s.env, unofficial_napi_profile_cpu, &started,
                                              &profile),
                napi_ok);
      ASSERT_EQ(started, unofficial_napi_profile_start_ok);
      EXPECT_EQ(RunJsInt(s.env, "work(2)"), 1);
      napi_value json = nullptr;
      ASSERT_EQ(unofficial_napi_profile_stop(s.env, profile, &json), napi_ok);
    }
    // Left running: env teardown stops this one, after the env's session.
    unofficial_napi_profile running_profile = nullptr;
    ASSERT_EQ(unofficial_napi_profile_start(s.env, unofficial_napi_profile_cpu, &started,
                                            &running_profile),
              napi_ok);
    ASSERT_EQ(started, unofficial_napi_profile_start_ok);
    EXPECT_EQ(RunJsInt(s.env, "work()"), 1);
  }
  std::vector<Session> sessions;
  ASSERT_TRUE(FindSessions(dir.path(), &sessions));
  ASSERT_EQ(sessions.size(), 1u);
  EXPECT_EQ(CheckCompleteSession(sessions[0]), "ok");
}

// V8 handles the samples it takes on the env's thread (CollectSample's, and
// one when a profile starts) before the sampler thread's earlier ones that it
// hasn't handled yet, so its samples are not in time order. Taking samples on
// the env's thread nonstop makes that happen in nearly every run.
TEST_F(Test76EnvCpuProfile, WritesSamplesInTimeOrder) {
  if (!ReserveSessions(1)) GTEST_SKIP() << kRunAlone;
  TempDir dir;
  {
    ScopedProfileDir set(dir.path().c_str());
    EnvScope s(runtime_.get());
    DefineWork(s.env);
    EXPECT_EQ(RunJsInt(s.env,
                       "const end = Date.now() + 200;"
                       "while (Date.now() < end) collectSample();"
                       "work()"),
              1);
  }
  std::vector<Session> sessions;
  ASSERT_TRUE(FindSessions(dir.path(), &sessions));
  ASSERT_EQ(sessions.size(), 1u);
  EXPECT_EQ(CheckCompleteSession(sessions[0]), "ok");
}

// Past 2^20 samples, V8 records no more: the profile is written, but it may
// have lost samples, so it is not marked valid.
TEST_F(Test76EnvCpuProfile, WritesAProfileAtTheSampleLimitButNotAsValid) {
  if (!ReserveSessions(1)) GTEST_SKIP() << kRunAlone;
  TempDir dir;
  {
    ScopedProfileDir set(dir.path().c_str());
    EnvScope s(runtime_.get());
    DefineWork(s.env);
    EXPECT_EQ(RunJsInt(s.env,
                       "for (let i = 0; i < 1100000; i++) collectSample();"
                       "work()"),
              1);
  }
  std::vector<Session> sessions;
  ASSERT_TRUE(FindSessions(dir.path(), &sessions));
  ASSERT_EQ(sessions.size(), 1u);
  EXPECT_EQ(CheckWrittenSession(sessions[0], "sample_limit_reached"), "ok");
}

TEST_F(Test76EnvCpuProfile, NeverOverwritesAnExistingFile) {
  if (!ReserveSessions(2)) GTEST_SKIP() << kRunAlone;
  TempDir dir;
  ScopedProfileDir set(dir.path().c_str());
  // The first env's file names give the next env's: same pid and process
  // token, next ordinal.
  { EnvScope s(runtime_.get()); }
  std::vector<Session> sessions;
  ASSERT_TRUE(FindSessions(dir.path(), &sessions));
  ASSERT_EQ(sessions.size(), 1u);
  const std::string first = sessions[0].profile.filename().string();
  const std::string prefix = first.substr(0, first.rfind('-') + 1);
  const fs::path taken = dir.path() / (prefix + std::to_string(sessions[0].ordinal + 1) +
                                       ".cpuprofile");
  { std::ofstream(taken) << "taken"; }
  const fs::path record = dir.path() / (prefix + std::to_string(sessions[0].ordinal + 1) +
                                        ".metadata.json");
  {
    EnvScope s(runtime_.get());
    EXPECT_EQ(RunJsInt(s.env, "6 * 7"), 42);
  }
  EXPECT_EQ(ReadFile(taken), "taken");
  // The record file was created before the profile's name was found taken,
  // so the refusal is recorded there.
  ASSERT_TRUE(fs::exists(record));
  EXPECT_EQ(CheckRefusedRecord(ReadFile(record), "output_open_failed", {EEXIST}), "ok");
}

TEST_F(Test76EnvCpuProfile, ReportsAMissingDirectoryOnStderr) {
  if (!ReserveSessions(1)) GTEST_SKIP() << kRunAlone;
  TempDir dir;
  const fs::path missing = dir.path() / "missing";
  const std::vector<std::string> records = RunRefusedEnv(runtime_.get(), missing.string());
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(CheckRefusedRecord(records[0], "directory_open_failed", {ENOENT}), "ok");
  EXPECT_FALSE(fs::exists(missing));
}

TEST_F(Test76EnvCpuProfile, ReportsAnUnwritableDirectoryOnStderr) {
  if (geteuid() == 0) GTEST_SKIP() << "root can write to a read-only directory";
  if (!ReserveSessions(1)) GTEST_SKIP() << kRunAlone;
  TempDir dir;
  ASSERT_EQ(chmod(dir.path().c_str(), 0500), 0);
  const std::vector<std::string> records = RunRefusedEnv(runtime_.get(), dir.path().string());
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(CheckRefusedRecord(records[0], "output_open_failed", {EACCES}), "ok");
  EXPECT_TRUE(ListDir(dir.path()).empty());
}

TEST_F(Test76EnvCpuProfile, RefusesARelativeDirectory) {
  if (!ReserveSessions(1)) GTEST_SKIP() << kRunAlone;
  const std::string relative = "napi-cpu-profile-relative";
  const std::vector<std::string> records = RunRefusedEnv(runtime_.get(), relative);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(CheckRefusedRecord(records[0], "directory_not_absolute", {0}), "ok");
  EXPECT_FALSE(fs::exists(relative));
}

// The directory may be a symbolic link (macOS's /tmp is one), however it is
// spelled.
TEST_F(Test76EnvCpuProfile, WritesThroughASymbolicLinkToTheDirectory) {
  if (!ReserveSessions(2)) GTEST_SKIP() << kRunAlone;
  TempDir dir;
  const fs::path real = dir.path() / "real";
  const fs::path link = dir.path() / "link";
  ASSERT_TRUE(fs::create_directory(real));
  fs::create_directory_symlink(real, link);
  for (const std::string& value : {link.string(), link.string() + "/"}) {
    ScopedProfileDir set(value.c_str());
    EnvScope s(runtime_.get());
    DefineWork(s.env);
    EXPECT_EQ(RunJsInt(s.env, "work()"), 1);
  }
  std::vector<Session> sessions;
  ASSERT_TRUE(FindSessions(real, &sessions));
  ASSERT_EQ(sessions.size(), 2u);
  for (const Session& session : sessions) EXPECT_EQ(CheckCompleteSession(session), "ok");
}

TEST_F(Test76EnvCpuProfile, WritesNothingWhenTheVariableIsUnsetOrEmpty) {
  for (const char* value : {static_cast<const char*>(nullptr), ""}) {
    ScopedProfileDir set(value);
    StderrCapture capture;
    {
      EnvScope s(runtime_.get());
      DefineWork(s.env);
      EXPECT_EQ(RunJsInt(s.env, "work()"), 1);
    }
    const std::vector<std::string> records = capture.Stop();
    EXPECT_TRUE(records.empty()) << (value == nullptr ? "unset: " : "empty: ") << records[0];
  }
}

TEST_F(Test76EnvCpuProfile, StopsAfterTheSessionLimit) {
  if (g_sessions_used > 0) GTEST_SKIP() << kRunAlone;
  g_sessions_used = kMaxSessions + 2;
  TempDir dir;
  std::vector<std::string> records;
  {
    ScopedProfileDir set(dir.path().c_str());
    StderrCapture capture;
    // The first env past the limit says so on stderr, and later ones say
    // nothing. Every env runs as usual.
    for (int i = 0; i < kMaxSessions + 2; ++i) {
      EnvScope s(runtime_.get());
      EXPECT_EQ(RunJsInt(s.env, "6 * 7"), 42);
    }
    records = capture.Stop();
  }
  std::vector<Session> sessions;
  ASSERT_TRUE(FindSessions(dir.path(), &sessions));
  ASSERT_EQ(sessions.size(), static_cast<size_t>(kMaxSessions));
  for (int i = 0; i < kMaxSessions; ++i) {
    EXPECT_EQ(sessions[i].ordinal, static_cast<unsigned>(i + 1));
  }
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(CheckInJs(R"JS((() => {
    const parsed = JSON.parse(record);
    if (parsed.status !== 'session_limit_reached' || parsed.ordinal !== 9 ||
        parsed.valid !== false || parsed.maxSessions !== 8) {
      throw new Error('record: ' + record);
    }
    return 'ok';
  })())JS",
                      {{"record", records[0]}}),
            "ok");
}
