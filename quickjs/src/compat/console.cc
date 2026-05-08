#include "compat/console.h"

namespace quickjs::detail
{
    // Brief: RepairBootstrapConsoleBindings belongs to the console compatibility layer.
    // It rebinds bootstrap console methods to the actual global console object.
    // Inputs stay as N-API handles owned by the caller.
    // Missing console or require support is treated as a no-op for early bootstrap contexts.
    // Any pending exception is cleared because this repair must not abort env creation.
    void RepairBootstrapConsoleBindings(napi_env env)
    {
        if (env == nullptr)
            return;

        static const char kRepairSource[] =
            "(() => {"
            "  const consoleObject = globalThis.console;"
            "  if (consoleObject == null) return;"
            "  if (typeof require !== 'function') return;"
            "  const { Console } = require('console');"
            "  if (Console == null || Console.prototype == null) return;"
            "  for (const key of Reflect.ownKeys(Console.prototype)) {"
            "    if (key === 'constructor') continue;"
            "    const desc = Reflect.getOwnPropertyDescriptor(Console.prototype, key);"
            "    if (desc == null) continue;"
            "    if (typeof desc.value === 'function') {"
            "      desc.value = desc.value.bind(consoleObject);"
            "      try {"
            "        Object.defineProperty(desc.value, 'name', { value: desc.value.name, configurable: true });"
            "      } catch {}"
            "    }"
            "    Object.defineProperty(consoleObject, key, desc);"
            "  }"
            "})()";

        napi_value source = nullptr;
        napi_value result = nullptr;
        if (napi_create_string_utf8(env, kRepairSource, NAPI_AUTO_LENGTH, &source) != napi_ok ||
            source == nullptr ||
            napi_run_script(env, source, &result) != napi_ok)
        {
            bool has_pending = false;
            if (napi_is_exception_pending(env, &has_pending) == napi_ok && has_pending)
            {
                napi_value ignored = nullptr;
                (void)napi_get_and_clear_last_exception(env, &ignored);
            }
        }
    }
}
