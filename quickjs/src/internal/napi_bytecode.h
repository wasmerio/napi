#ifndef NAPI_QUICKJS_INTERNAL_NAPI_BYTECODE_H_
#define NAPI_QUICKJS_INTERNAL_NAPI_BYTECODE_H_

#include <quickjs.h>

#define XXH_INLINE_ALL
#include "internal/xxhash.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace quickjs::detail
{
    // QuickJS bytecode is self-contained and carries no source identity, so
    // serialized handles are prefixed with a hash of the source they were
    // compiled from; deserialize rejects bytes whose source differs (the
    // semantics V8 provides natively via CachedData validation).
    inline constexpr char k_bytecode_prefix_magic[4] = {'Q', 'J', 'S', 'C'};
    inline constexpr size_t k_bytecode_prefix_size = 12;

    inline uint64_t napi_bytecode_source_hash(const std::string &source)
    {
        return XXH3_64bits(source.data(), source.size());
    }

    inline void napi_bytecode_append_prefix(std::vector<uint8_t> *out, uint64_t source_hash)
    {
        out->insert(out->end(), k_bytecode_prefix_magic, k_bytecode_prefix_magic + 4);
        for (int i = 0; i < 8; ++i)
            out->push_back(static_cast<uint8_t>(source_hash >> (8 * i)));
    }

    // Returns the payload span past the prefix, or nullptr when the prefix is
    // absent or the source hash does not match.
    inline const uint8_t *napi_bytecode_validate_prefix(const uint8_t *bytes,
                                                        size_t byte_length,
                                                        uint64_t expected_source_hash,
                                                        size_t *payload_length_out)
    {
        *payload_length_out = 0;
        if (bytes == nullptr || byte_length <= k_bytecode_prefix_size)
            return nullptr;
        if (std::memcmp(bytes, k_bytecode_prefix_magic, 4) != 0)
            return nullptr;
        uint64_t stored = 0;
        for (int i = 0; i < 8; ++i)
            stored |= static_cast<uint64_t>(bytes[4 + i]) << (8 * i);
        if (stored != expected_source_hash)
            return nullptr;
        *payload_length_out = byte_length - k_bytecode_prefix_size;
        return bytes + k_bytecode_prefix_size;
    }
    // Backing store for an unofficial_napi bytecode handle (see
    // unofficial_napi_js_source). Created/owned via the
    // unofficial_napi_bytecode_* APIs implemented by napi_contextify__ and
    // consumed by JSSource-accepting APIs (contextify + module_wrap).
    struct napi_bytecode_record__
    {
        JSContext *ctx = nullptr;
        std::vector<uint8_t> bytes;
        std::string source_utf8;
        std::string filename_utf8;
        int32_t shape = 0;
        std::vector<std::string> params;
        int32_t line_offset = 0;
        int32_t column_offset = 0;
        // Live artifact per shape: script -> the compiled function-bytecode
        // value (dup before JS_EvalFunction, which consumes its argument);
        // cjs_function -> the compiled function; module -> the module value.
        JSValue artifact = JS_UNDEFINED;
    };
}

#endif  // NAPI_QUICKJS_INTERNAL_NAPI_BYTECODE_H_
