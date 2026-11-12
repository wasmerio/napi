#ifndef NAPI_QUICKJS_INTERNAL_NAPI_LAZY_PROPERTIES_H_
#define NAPI_QUICKJS_INTERNAL_NAPI_LAZY_PROPERTIES_H_

#include "unofficial_napi.h"

namespace quickjs::detail
{
    napi_status define_lazy_data_properties(napi_env env,
                                            napi_value object,
                                            napi_value names,
                                            napi_value initializer,
                                            napi_property_attributes attributes);

    napi_status is_lazy_data_property(napi_env env,
                                      napi_value object,
                                      napi_value key,
                                      bool *result_out);
}

#endif // NAPI_QUICKJS_INTERNAL_NAPI_LAZY_PROPERTIES_H_
