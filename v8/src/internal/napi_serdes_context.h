#ifndef NAPI_V8_SERDES_CONTEXT_H_
#define NAPI_V8_SERDES_CONTEXT_H_

#include "internal/napi_v8_agent.h"
#include <cstdint>
#include <memory>
#include <vector>

#include <v8.h>

#include "js_native_api.h"

namespace v8impl::detail {

class SerializerContext : public v8::ValueSerializer::Delegate {
 public:
  SerializerContext(napi_env env, v8::Local<v8::Object> wrap);
  ~SerializerContext() override;

  void ThrowDataCloneError(v8::Local<v8::String> message) override;
  // The serializer's buffer grows here: charged to the isolate's host memory
  // (if any) before it is allocated. A refused growth fails the write, which
  // V8 reports as its out-of-memory clone error from writeValue and not at
  // all from the raw writes; refused_ remembers it for them.
  void* ReallocateBufferMemory(void* old_buffer, size_t size, size_t* actual_size) override;
  void FreeBufferMemory(void* buffer) override;
  v8::Maybe<uint32_t> GetSharedArrayBufferId(
      v8::Isolate* isolate,
      v8::Local<v8::SharedArrayBuffer> shared_array_buffer) override;
  v8::Maybe<bool> WriteHostObject(v8::Isolate* isolate,
                                  v8::Local<v8::Object> input) override;

  static void New(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void WriteHeader(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void WriteValue(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void SetTreatArrayBufferViewsAsHostObjects(
      const v8::FunctionCallbackInfo<v8::Value>& args);
  static void ReleaseBuffer(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void TransferArrayBuffer(
      const v8::FunctionCallbackInfo<v8::Value>& args);
  static void WriteUint32(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void WriteUint64(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void WriteDouble(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void WriteRawBytes(const v8::FunctionCallbackInfo<v8::Value>& args);

 private:
  static SerializerContext* Unwrap(
      const v8::FunctionCallbackInfo<v8::Value>& args);
  static void WeakCallback(
      const v8::WeakCallbackInfo<SerializerContext>& info);
  // Throws the out-of-memory clone error if a growth was refused.
  bool ThrowIfRefused();

  napi_env env_ = nullptr;
  v8::Isolate* isolate_ = nullptr;
  v8::Global<v8::Object> wrap_;
  // Declared before serializer_, which frees its buffer through
  // FreeBufferMemory when it is destroyed.
  NapiHostCharge charge_;
  size_t buffer_bytes_ = 0;
  bool refused_ = false;
  v8::ValueSerializer serializer_;
};

class DeserializerContext : public v8::ValueDeserializer::Delegate {
 public:
  DeserializerContext(napi_env env,
                      v8::Local<v8::Object> wrap,
                      v8::Local<v8::Value> buffer);
  ~DeserializerContext() override;

  v8::MaybeLocal<v8::Object> ReadHostObject(v8::Isolate* isolate) override;

  static void New(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void ReadHeader(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void ReadValue(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void TransferArrayBuffer(
      const v8::FunctionCallbackInfo<v8::Value>& args);
  static void GetWireFormatVersion(
      const v8::FunctionCallbackInfo<v8::Value>& args);
  static void ReadUint32(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void ReadUint64(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void ReadDouble(const v8::FunctionCallbackInfo<v8::Value>& args);
  static void ReadRawBytes(const v8::FunctionCallbackInfo<v8::Value>& args);

 private:
  static DeserializerContext* Unwrap(
      const v8::FunctionCallbackInfo<v8::Value>& args);
  static void WeakCallback(
      const v8::WeakCallbackInfo<DeserializerContext>& info);

  napi_env env_ = nullptr;
  v8::Isolate* isolate_ = nullptr;
  v8::Global<v8::Object> wrap_;
  // Pays for data_, the copy of the input, while the context lives.
  NapiHostCharge charge_;
  std::vector<uint8_t> data_;

 public:
  size_t data_size() const { return data_.size(); }
  std::unique_ptr<v8::ValueDeserializer> deserializer_;
};

}  // namespace v8impl::detail

#endif  // NAPI_V8_SERDES_CONTEXT_H_
