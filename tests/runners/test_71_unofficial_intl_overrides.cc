// unofficial_napi_set_process_time_zone, unofficial_napi_refresh_intl_defaults
// and unofficial_napi_set_default_locale.
//
// An embedder that emulates another time zone or locale (DevTools'
// Emulation.setTimezoneOverride and Emulation.setLocaleOverride) needs Date
// and Intl to agree on it. V8's Intl reads ICU's process default time zone,
// while each isolate keeps a date cache built from it: the time zone is one
// per process, and an isolate takes a change at its next event-loop
// checkpoint (or refresh). The default locale is per isolate. QuickJS has no
// ICU and supports neither.
//
// The time zone is process-wide: every V8 test gives the host's back.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "test_env.h"

// The V8 provider builds the overrides only when v8/CMakeLists.txt finds ICU's
// C API in the V8 library; it passes NAPI_ICU_SUFFIX 0 when it found none.
#if defined(NAPI_TEST_ENGINE_V8) && !(defined(NAPI_ICU_SUFFIX) && NAPI_ICU_SUFFIX == 0)
#define TEST71_V8_WITH_ICU 1
#else
#define TEST71_V8_WITH_ICU 0
#endif

class Test71UnofficialIntlOverrides : public FixtureTestBase {
 protected:
#if TEST71_V8_WITH_ICU
  void TearDown() override {
    EXPECT_EQ(unofficial_napi_set_process_time_zone(nullptr, 0), napi_ok);
  }
#endif
};

namespace {

napi_status SetZone(const char* zone) {
  return unofficial_napi_set_process_time_zone(zone, zone == nullptr ? 0 : std::strlen(zone));
}

napi_status SetLocale(napi_env env, const char* tag) {
  return unofficial_napi_set_default_locale(env, tag, tag == nullptr ? 0 : std::strlen(tag));
}

}  // namespace

#if TEST71_V8_WITH_ICU

// ICU's C API in the V8 library, with the provider's NAPI_ICU_SUFFIX
// (tests/CMakeLists.txt), to give the process a known default locale.
#if defined(NAPI_ICU_SUFFIX) && NAPI_ICU_SUFFIX
#define TEST_ICU_PASTE(name, suffix) name##_##suffix
#define TEST_ICU_EXPAND(name, suffix) TEST_ICU_PASTE(name, suffix)
#define TEST_ICU(name) TEST_ICU_EXPAND(name, NAPI_ICU_SUFFIX)
extern "C" {
void TEST_ICU(uloc_setDefault)(const char* locale_id, int* status);
const char* TEST_ICU(uloc_getDefault)(void);
}
#endif

namespace {

std::string Eval(napi_env env, const char* source_text) {
  napi_value source = nullptr;
  if (napi_create_string_utf8(env, source_text, NAPI_AUTO_LENGTH, &source) != napi_ok) {
    return "<error>";
  }
  napi_value result = nullptr;
  if (napi_run_script(env, source, &result) != napi_ok) {
    napi_value exception = nullptr;
    (void)napi_get_and_clear_last_exception(env, &exception);
    return "<exception>";
  }
  napi_value as_string = nullptr;
  if (napi_coerce_to_string(env, result, &as_string) != napi_ok) return "<error>";
  char buffer[256] = {0};
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, as_string, buffer, sizeof(buffer), &copied) != napi_ok) {
    return "<error>";
  }
  return std::string(buffer, copied);
}

constexpr char kIntlTimeZone[] = "Intl.DateTimeFormat().resolvedOptions().timeZone";
// Minutes from local time to UTC on 2021-01-15; the zones below have no DST.
constexpr char kDateOffset[] = "new Date(Date.UTC(2021, 0, 15, 12)).getTimezoneOffset()";
constexpr char kDateHours[] = "new Date(Date.UTC(2021, 0, 15, 12)).getHours()";
constexpr char kNumberLocale[] = "new Intl.NumberFormat().resolvedOptions().locale";
constexpr char kDateTimeLocale[] = "new Intl.DateTimeFormat().resolvedOptions().locale";

napi_status Checkpoint(napi_env env) {
  return unofficial_napi_event_loop_checkpoint(
      env, unofficial_napi_event_loop_checkpoint_microtasks, false, nullptr);
}

// Evaluates source in a new env (a new isolate) on its own thread, so the
// test's own env stays the current isolate on this one.
std::string EvalInNewEnv(const char* source_text) {
  std::string result = "<no env>";
  std::thread thread([&] {
    EnvScope fresh(nullptr);
    if (fresh.env != nullptr) result = Eval(fresh.env, source_text);
  });
  thread.join();
  return result;
}

// Sets TZ, and puts it back as it was when destroyed: before TearDown, which
// re-detects the host's zone from it.
class ScopedTz {
 public:
  explicit ScopedTz(const char* value) {
    const char* old = std::getenv("TZ");
    had_ = old != nullptr;
    if (had_) old_ = old;
    set_ = setenv("TZ", value, 1) == 0;
  }
  ~ScopedTz() {
    if (had_) {
      setenv("TZ", old_.c_str(), 1);
    } else {
      unsetenv("TZ");
    }
  }
  bool set() const { return set_; }

 private:
  bool had_ = false;
  bool set_ = false;
  std::string old_;
};

// Calls body with an env of its own, on its own thread, over and over until
// destroyed.
class LoopInNewEnv {
 public:
  template <typename Body>
  explicit LoopInNewEnv(Body body)
      : thread_([this, body] {
          EnvScope env(nullptr);
          ready_.store(true);
          while (!stop_.load()) body(env.env);
        }) {
    while (!ready_.load()) std::this_thread::yield();
  }
  ~LoopInNewEnv() {
    stop_.store(true);
    thread_.join();
  }

 private:
  std::atomic<bool> ready_{false};
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace

TEST_F(Test71UnofficialIntlOverrides, TimeZoneReachesDateAndIntlAtTheNextCheckpoint) {
  EnvScope s(runtime_.get());
  const std::string host_zone = Eval(s.env, kIntlTimeZone);
  // Also fills the isolate's date cache with the host's zone.
  const std::string host_offset = Eval(s.env, kDateOffset);
  ASSERT_NE(host_zone, "<exception>");

  ASSERT_EQ(SetZone("Pacific/Honolulu"), napi_ok);
  ASSERT_EQ(Checkpoint(s.env), napi_ok);
  EXPECT_EQ(Eval(s.env, kIntlTimeZone), "Pacific/Honolulu");
  EXPECT_EQ(Eval(s.env, kDateOffset), "600");
  EXPECT_EQ(Eval(s.env, kDateHours), "2");

  // unofficial_napi_refresh_intl_defaults takes a change without a checkpoint.
  ASSERT_EQ(SetZone("Asia/Tokyo"), napi_ok);
  ASSERT_EQ(unofficial_napi_refresh_intl_defaults(s.env), napi_ok);
  EXPECT_EQ(Eval(s.env, kIntlTimeZone), "Asia/Tokyo");
  EXPECT_EQ(Eval(s.env, kDateOffset), "-540");
  EXPECT_EQ(Eval(s.env, kDateHours), "21");

  // A TZ change notification does not re-detect the host's zone over it.
  ASSERT_EQ(unofficial_napi_notify_datetime_configuration_change(s.env), napi_ok);
  EXPECT_EQ(Eval(s.env, kIntlTimeZone), "Asia/Tokyo");
  EXPECT_EQ(Eval(s.env, kDateOffset), "-540");

  // A new isolate starts with it.
  EXPECT_EQ(EvalInNewEnv(kIntlTimeZone), "Asia/Tokyo");
  EXPECT_EQ(EvalInNewEnv(kDateOffset), "-540");

  // Length 0: the host's zone again.
  ASSERT_EQ(SetZone(nullptr), napi_ok);
  ASSERT_EQ(Checkpoint(s.env), napi_ok);
  EXPECT_EQ(Eval(s.env, kIntlTimeZone), host_zone);
  EXPECT_EQ(Eval(s.env, kDateOffset), host_offset);
}

TEST_F(Test71UnofficialIntlOverrides, UnknownTimeZoneIsInvalidAndChangesNothing) {
  EnvScope s(runtime_.get());
  ASSERT_EQ(SetZone("Pacific/Honolulu"), napi_ok);
  ASSERT_EQ(Checkpoint(s.env), napi_ok);

  EXPECT_EQ(SetZone("Not/A_Zone"), napi_invalid_arg);
  EXPECT_EQ(SetZone("Etc/Unknown"), napi_invalid_arg);
  EXPECT_EQ(SetZone("Asia/ Tokyo"), napi_invalid_arg);
  // ICU takes custom ids, but they are not IANA ids.
  EXPECT_EQ(SetZone("GMT+05:00"), napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_set_process_time_zone(nullptr, 5), napi_invalid_arg);

  ASSERT_EQ(Checkpoint(s.env), napi_ok);
  EXPECT_EQ(Eval(s.env, kIntlTimeZone), "Pacific/Honolulu");
  EXPECT_EQ(Eval(s.env, kDateOffset), "600");
  EXPECT_EQ(EvalInNewEnv(kIntlTimeZone), "Pacific/Honolulu");
}

// For a POSIX TZ that names no IANA zone, ICU detects a zone that has no id
// to rebuild it from. Clearing the override gives that zone back.
TEST_F(Test71UnofficialIntlOverrides, ClearingGivesBackAPosixTzZone) {
  EnvScope s(runtime_.get());
  // UTC-5 with no DST, under a name that no IANA zone has.
  ScopedTz tz("FOO5");
  ASSERT_TRUE(tz.set());
  ASSERT_EQ(unofficial_napi_notify_datetime_configuration_change(s.env), napi_ok);
  const std::string host_zone = Eval(s.env, kIntlTimeZone);
  const std::string host_offset = Eval(s.env, kDateOffset);

  ASSERT_EQ(SetZone("Asia/Tokyo"), napi_ok);
  ASSERT_EQ(Checkpoint(s.env), napi_ok);
  EXPECT_EQ(Eval(s.env, kDateOffset), "-540");

  ASSERT_EQ(SetZone(nullptr), napi_ok);
  ASSERT_EQ(Checkpoint(s.env), napi_ok);
  EXPECT_EQ(Eval(s.env, kIntlTimeZone), host_zone);
  EXPECT_EQ(Eval(s.env, kDateOffset), host_offset);
}

// A TZ notification on another thread that found no override must not
// re-detect the host's zone after set_process_time_zone has returned.
TEST_F(Test71UnofficialIntlOverrides, NotifyDoesNotUndoAConcurrentOverride) {
  EnvScope s(runtime_.get());
  // A zone the host is not already in, or a re-detection would not show.
  const std::string host_zone = Eval(s.env, kIntlTimeZone);
  const char* zone = host_zone == "Asia/Tokyo" ? "Pacific/Honolulu" : "Asia/Tokyo";
  int lost = 0;
  {
    LoopInNewEnv notifier([](napi_env env) {
      (void)unofficial_napi_notify_datetime_configuration_change(env);
    });
    for (int i = 0; i < 500; ++i) {
      if (SetZone(nullptr) != napi_ok || SetZone(zone) != napi_ok) {
        ++lost;
        continue;
      }
      // Long enough for a re-detection that was under way to land.
      std::this_thread::sleep_for(std::chrono::microseconds(200));
      if (Eval(s.env, kIntlTimeZone) != zone) ++lost;
    }
  }
  EXPECT_EQ(lost, 0);
}

TEST_F(Test71UnofficialIntlOverrides, DefaultLocaleIsPerIsolate) {
  EnvScope s(runtime_.get());
  const std::string process_locale = Eval(s.env, kNumberLocale);
  const std::string process_date_locale = Eval(s.env, kDateTimeLocale);
  ASSERT_NE(process_locale, "<exception>");
  // A locale the host is not already using.
  const char* tag = process_locale == "de-DE" ? "fr-FR" : "de-DE";

  ASSERT_EQ(SetLocale(s.env, tag), napi_ok);
  EXPECT_EQ(Eval(s.env, kNumberLocale), tag);
  EXPECT_EQ(Eval(s.env, kDateTimeLocale), tag);
  EXPECT_EQ(Eval(s.env, "new Intl.Collator().resolvedOptions().locale"), tag);

  // Other isolates keep the process default.
  EXPECT_EQ(EvalInNewEnv(kNumberLocale), process_locale);

  // A malformed tag changes nothing.
  EXPECT_EQ(SetLocale(s.env, "de_DE"), napi_invalid_arg);
  EXPECT_EQ(SetLocale(s.env, "!!"), napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_set_default_locale(s.env, nullptr, 2), napi_invalid_arg);
  EXPECT_EQ(Eval(s.env, kNumberLocale), tag);

  // "und", the root locale, is well formed.
  EXPECT_EQ(SetLocale(s.env, "und"), napi_ok);
  EXPECT_EQ(Eval(s.env, kNumberLocale), "und");

  // Length 0: the process default again.
  ASSERT_EQ(SetLocale(s.env, nullptr), napi_ok);
  EXPECT_EQ(Eval(s.env, kNumberLocale), process_locale);
  EXPECT_EQ(Eval(s.env, kDateTimeLocale), process_date_locale);
}

// ICU names a locale differently from the id of its tag (eng-US is eng_US,
// kept as en_US). A tag that names the process default replaces an earlier
// override, as any other tag does.
TEST_F(Test71UnofficialIntlOverrides, TagsAreTakenUnderIcusName) {
#if defined(NAPI_ICU_SUFFIX) && NAPI_ICU_SUFFIX
  // en_US as the process default, and the old one back after the env is gone.
  const std::string old_default = TEST_ICU(uloc_getDefault)();
  struct Restore {
    const std::string& id;
    ~Restore() {
      int status = 0;
      TEST_ICU(uloc_setDefault)(id.c_str(), &status);
    }
  } restore{old_default};
  int status = 0;
  TEST_ICU(uloc_setDefault)("en_US", &status);
  ASSERT_STREQ(TEST_ICU(uloc_getDefault)(), "en_US");

  EnvScope s(runtime_.get());
  ASSERT_EQ(Eval(s.env, kNumberLocale), "en-US");
  for (const char* tag : {"eng-US", "en-US", "EN-us"}) {
    ASSERT_EQ(SetLocale(s.env, "de-DE"), napi_ok);
    ASSERT_EQ(Eval(s.env, kNumberLocale), "de-DE");
    EXPECT_EQ(SetLocale(s.env, tag), napi_ok) << tag;
    EXPECT_EQ(Eval(s.env, kNumberLocale), "en-US") << tag;
  }
  // Other ids that ICU renames.
  EXPECT_EQ(SetLocale(s.env, "deu-DE"), napi_ok);
  EXPECT_EQ(Eval(s.env, kNumberLocale), "de-DE");
  EXPECT_EQ(SetLocale(s.env, "root"), napi_ok);
  EXPECT_EQ(Eval(s.env, kNumberLocale), "und");
  EXPECT_EQ(SetLocale(s.env, "und-u-ca-buddhist"), napi_ok);
  EXPECT_EQ(Eval(s.env, "new Intl.DateTimeFormat().resolvedOptions().calendar"), "buddhist");

  ASSERT_EQ(SetLocale(s.env, nullptr), napi_ok);
  EXPECT_EQ(Eval(s.env, kNumberLocale), "en-US");
  EXPECT_STREQ(TEST_ICU(uloc_getDefault)(), "en_US");
#else
  GTEST_SKIP() << "built without the provider's NAPI_ICU_SUFFIX";
#endif
}

// A tag's ICU id can be longer than the tag (-u-ca- becomes @calendar=,
// phonebk phonebook): tags of up to 256 bytes are taken whole.
TEST_F(Test71UnofficialIntlOverrides, LongTagsAreTakenWhole) {
  EnvScope s(runtime_.get());
  const std::string process_locale = Eval(s.env, kNumberLocale);
  const std::string base = process_locale == "de-DE" ? "fr-FR" : "de-DE";

  // 140 bytes, and an ICU id of over 230.
  const std::string extensions =
      base +
      "-u-ca-buddhist-co-phonebk-nu-arab-hc-h23-fw-mon-ms-metric-kn-true-kf-upper-ka-shifted"
      "-kb-true-kc-true-cu-usd-tz-usnyc-rg-uszzzz-sd-usny";
  ASSERT_EQ(SetLocale(s.env, extensions.c_str()), napi_ok);
  EXPECT_EQ(Eval(s.env, kNumberLocale), base + "-u-nu-arab");
  EXPECT_EQ(Eval(s.env, "new Intl.DateTimeFormat().resolvedOptions().calendar"), "buddhist");

  // 256 bytes of private use, and an ICU id as long.
  std::string private_use = base + "-x";
  for (int i = 0; i < 27; ++i) private_use += "-abcdefgh";
  private_use += "-abcde";
  ASSERT_EQ(private_use.size(), 256u);
  ASSERT_EQ(SetLocale(s.env, private_use.c_str()), napi_ok);
  EXPECT_EQ(Eval(s.env, kNumberLocale), base);
  // One byte more is longer than any tag.
  EXPECT_EQ(SetLocale(s.env, (private_use + "f").c_str()), napi_invalid_arg);

  ASSERT_EQ(SetLocale(s.env, nullptr), napi_ok);
  EXPECT_EQ(Eval(s.env, kNumberLocale), process_locale);
}

// One thread keeps swapping a locale in for its own isolate while isolates
// are created on another: each new isolate caches the process default.
TEST_F(Test71UnofficialIntlOverrides, NewIsolatesDoNotCacheAnotherIsolatesLocale) {
  EnvScope s(runtime_.get());
  const std::string process_locale = Eval(s.env, kNumberLocale);
  const char* tag = process_locale == "de-DE" ? "fr-FR" : "de-DE";
  int wrong = 0;
  {
    LoopInNewEnv swapper([tag](napi_env env) {
      (void)SetLocale(env, tag);
      (void)SetLocale(env, nullptr);
    });
    for (int i = 0; i < 200; ++i) {
      if (EvalInNewEnv(kNumberLocale) != process_locale) ++wrong;
    }
  }
  EXPECT_EQ(wrong, 0);
}

#endif  // TEST71_V8_WITH_ICU

#if defined(NAPI_TEST_ENGINE_QUICKJS) || (defined(NAPI_TEST_ENGINE_V8) && !TEST71_V8_WITH_ICU)
// No ICU (QuickJS, or a V8 library without ICU's C API): the overrides are
// unsupported, a zone change is never pending, and malformed calls are still
// napi_invalid_arg.
TEST_F(Test71UnofficialIntlOverrides, UnsupportedWithoutIcu) {
  EnvScope s(runtime_.get());
  EXPECT_EQ(SetZone("Asia/Tokyo"), napi_generic_failure);
  EXPECT_EQ(SetZone(nullptr), napi_generic_failure);
  EXPECT_EQ(unofficial_napi_set_process_time_zone(nullptr, 5), napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_refresh_intl_defaults(s.env), napi_ok);
  EXPECT_EQ(unofficial_napi_refresh_intl_defaults(nullptr), napi_invalid_arg);
  EXPECT_EQ(SetLocale(s.env, "de-DE"), napi_generic_failure);
  EXPECT_EQ(SetLocale(s.env, nullptr), napi_generic_failure);
  EXPECT_EQ(unofficial_napi_set_default_locale(s.env, nullptr, 2), napi_invalid_arg);
  EXPECT_EQ(unofficial_napi_set_default_locale(nullptr, "de", 2), napi_invalid_arg);
}
#endif  // NAPI_TEST_ENGINE_QUICKJS or V8 without ICU
