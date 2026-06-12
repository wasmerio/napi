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
    // QuickJS serialized bytecode is self-validating, mirroring what V8's
    // CachedData provides natively: every payload this provider emits starts
    // with a 20-byte header [QJSB magic, filename XXH3-64, payload XXH3-64].
    // - payload_hash guards corruption/truncation (JS_ReadObject is not
    //   hardened against bad input).
    // - filename_hash rejects payloads compiled under another name (QuickJS
    //   bytecode embeds the compile-time filename/URL — import.meta.url and
    //   stack traces would silently go stale). A stored hash of 0 means
    //   "unenforced": vm.SourceTextModule#createCachedData writes 0 because
    //   Node gives vm modules numbered default identifiers (vm:module(N))
    //   and V8 does not key its caches on the name either.
    // Source identity is NOT covered here — containers (sidecars/builtins)
    // and the edge.js vm cachedData wrapper validate it before bytes reach
    // this provider.
    inline constexpr char k_bytecode_payload_magic[4] = {'Q', 'J', 'S', 'B'};
    inline constexpr size_t k_bytecode_payload_header_size = 20;

    inline uint64_t napi_bytecode_hash64(const void *data, size_t size)
    {
        return XXH3_64bits(data, size);
    }

    inline void napi_bytecode_append_payload_header(std::vector<uint8_t> *out,
                                                    uint64_t filename_hash,
                                                    uint64_t payload_hash)
    {
        out->insert(out->end(), k_bytecode_payload_magic, k_bytecode_payload_magic + 4);
        for (int i = 0; i < 8; ++i)
            out->push_back(static_cast<uint8_t>(filename_hash >> (8 * i)));
        for (int i = 0; i < 8; ++i)
            out->push_back(static_cast<uint8_t>(payload_hash >> (8 * i)));
    }

    // Returns the raw JS_WriteObject span past the header, or nullptr when
    // the header is absent, the payload hash mismatches (corruption), or the
    // stored filename hash is non-zero and differs from expected.
    inline const uint8_t *napi_bytecode_validate_payload_header(const uint8_t *bytes,
                                                                size_t byte_length,
                                                                uint64_t expected_filename_hash,
                                                                size_t *payload_length_out)
    {
        *payload_length_out = 0;
        if (bytes == nullptr || byte_length <= k_bytecode_payload_header_size)
            return nullptr;
        if (std::memcmp(bytes, k_bytecode_payload_magic, 4) != 0)
            return nullptr;
        uint64_t stored_filename = 0;
        uint64_t stored_payload = 0;
        for (int i = 0; i < 8; ++i)
            stored_filename |= static_cast<uint64_t>(bytes[4 + i]) << (8 * i);
        for (int i = 0; i < 8; ++i)
            stored_payload |= static_cast<uint64_t>(bytes[12 + i]) << (8 * i);
        if (stored_filename != 0 && stored_filename != expected_filename_hash)
            return nullptr;
        const uint8_t *payload = bytes + k_bytecode_payload_header_size;
        const size_t payload_length = byte_length - k_bytecode_payload_header_size;
        if (stored_payload != napi_bytecode_hash64(payload, payload_length))
            return nullptr;
        *payload_length_out = payload_length;
        return payload;
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
