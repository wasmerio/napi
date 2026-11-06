// Regression test: name and description lengths above INT_MAX are refused.
//
// V8 takes a string's length as an int. napi_create_function,
// napi_define_class, node_api_symbol_for and
// unofficial_napi_create_private_symbol cast their size_t length to int with
// no check. A length of 2^31 or more turned negative, and V8 then ran strlen
// over the name: past the end of a name with no NUL, and into V8's length
// CHECK, which aborts the process, for a NUL more than 2 GiB away. A WASIX
// guest with 2 GiB of memory could pass such a length. Lengths of 2^32 or
// more wrapped instead and named the result with the wrong bytes. Node
// refuses all of them with napi_invalid_arg, as napi_create_string_utf8
// already did here.
//
// No buffer that large is needed: the length is refused before anything is
// read. The name below is short and NUL-terminated, so that without the check
// the strlen stays inside it and the test fails instead of crashing.
//
// V8 provider only: QuickJS takes the length as a size_t.

#include <climits>
#include <cstdint>
#include <vector>

#include "test_env.h"

class Test74NameLengthLimits : public FixtureTestBase {};

namespace {

napi_value Noop(napi_env /*env*/, napi_callback_info /*info*/) { return nullptr; }

using MakeNamed = napi_status (*)(napi_env, const char*, size_t, napi_value*);

napi_status MakeFunction(napi_env env, const char* name, size_t length, napi_value* result) {
  return napi_create_function(env, name, length, Noop, nullptr, result);
}

napi_status MakeClass(napi_env env, const char* name, size_t length, napi_value* result) {
  return napi_define_class(env, name, length, Noop, nullptr, 0, nullptr, result);
}

napi_status MakeSymbolFor(napi_env env, const char* name, size_t length, napi_value* result) {
  return node_api_symbol_for(env, name, length, result);
}

napi_status MakePrivateSymbol(napi_env env,
                              const char* name,
                              size_t length,
                              napi_value* result) {
  return unofficial_napi_create_private_symbol(env, name, length, result);
}

struct Maker {
  const char* what;
  MakeNamed make;
};

constexpr Maker kMakers[] = {
    {"napi_create_function", MakeFunction},
    {"napi_define_class", MakeClass},
    {"node_api_symbol_for", MakeSymbolFor},
    {"unofficial_napi_create_private_symbol", MakePrivateSymbol},
};

constexpr size_t kIntMax = static_cast<size_t>(INT_MAX);

}  // namespace

TEST_F(Test74NameLengthLimits, LengthsAboveIntMaxAreRefused) {
  EnvScope s(runtime_.get());
  ASSERT_NE(s.env, nullptr);

  // INT_MAX + 1 and SIZE_MAX - 1 turn negative as an int. On 64-bit hosts,
  // 2^32 - 1 is -1 too (what NAPI_AUTO_LENGTH becomes), and 2^32 + 3 wraps
  // to 3.
  std::vector<size_t> lengths = {kIntMax + 1, SIZE_MAX - 1};
  if (sizeof(size_t) > sizeof(uint32_t)) {
    lengths.push_back(static_cast<size_t>(UINT64_C(0xFFFFFFFF)));
    lengths.push_back(static_cast<size_t>(UINT64_C(0x100000003)));
  }
  for (const Maker& maker : kMakers) {
    for (size_t length : lengths) {
      napi_value result = nullptr;
      EXPECT_EQ(maker.make(s.env, "abc", length, &result), napi_invalid_arg)
          << maker.what << ", length " << length;
      EXPECT_EQ(result, nullptr) << maker.what << ", length " << length;
    }
  }
}

TEST_F(Test74NameLengthLimits, NullNamesWithLengthsAboveIntMax) {
  EnvScope s(runtime_.get());
  ASSERT_NE(s.env, nullptr);

  // As in Node, a function with a NULL name gets no name, whatever the
  // length. The others refuse a NULL name with a length.
  for (const Maker& maker : kMakers) {
    napi_value result = nullptr;
    const napi_status expected = maker.make == MakeFunction ? napi_ok : napi_invalid_arg;
    EXPECT_EQ(maker.make(s.env, nullptr, kIntMax + 1, &result), expected) << maker.what;
    EXPECT_EQ(result != nullptr, expected == napi_ok) << maker.what;
  }
}

TEST_F(Test74NameLengthLimits, LengthsUpToIntMaxStillReachV8) {
  EnvScope s(runtime_.get());
  ASSERT_NE(s.env, nullptr);

  for (const Maker& maker : kMakers) {
    napi_value result = nullptr;
    EXPECT_EQ(maker.make(s.env, "abc", 3, &result), napi_ok) << maker.what;
    EXPECT_NE(result, nullptr) << maker.what;
    result = nullptr;
    EXPECT_EQ(maker.make(s.env, "abc", NAPI_AUTO_LENGTH, &result), napi_ok) << maker.what;
    EXPECT_NE(result, nullptr) << maker.what;
    // INT_MAX itself passes the check. It is past V8's string length limit,
    // which V8 refuses before reading the name.
    result = nullptr;
    EXPECT_EQ(maker.make(s.env, "abc", kIntMax, &result), napi_generic_failure) << maker.what;
  }
}
