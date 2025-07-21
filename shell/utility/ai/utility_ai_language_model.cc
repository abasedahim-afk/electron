// Copyright (c) 2025 Microsoft, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/utility/ai/utility_ai_language_model.h"

#include "base/notimplemented.h"
#include "shell/browser/javascript_environment.h"
#include "shell/common/gin_converters/callback_converter.h"
#include "shell/common/gin_converters/std_converter.h"
#include "shell/common/gin_helper/dictionary.h"
#include "shell/common/gin_helper/event_emitter_caller.h"
#include "third_party/blink/public/mojom/ai/ai_common.mojom.h"
#include "third_party/blink/public/mojom/ai/ai_language_model.mojom.h"
#include "third_party/blink/public/mojom/ai/model_streaming_responder.mojom.h"

namespace gin {

template <>
struct Converter<on_device_model::mojom::ResponseConstraintPtr> {
  static v8::Local<v8::Value> ToV8(
      v8::Isolate* isolate,
      const on_device_model::mojom::ResponseConstraintPtr& val) {
    // TODO - A proper implementation here
    return v8::Undefined(isolate);
  }
};

template <>
struct Converter<blink::mojom::AILanguageModelPromptRole> {
  static v8::Local<v8::Value> ToV8(
      v8::Isolate* isolate,
      blink::mojom::AILanguageModelPromptRole value) {
    switch (value) {
      case blink::mojom::AILanguageModelPromptRole::kSystem:
        return StringToV8(isolate, "system");
      case blink::mojom::AILanguageModelPromptRole::kUser:
        return StringToV8(isolate, "user");
      case blink::mojom::AILanguageModelPromptRole::kAssistant:
        return StringToV8(isolate, "assistant");
      default:
        return StringToV8(isolate, "unknown");
    }
  }
};

template <>
struct Converter<blink::mojom::AILanguageModelPromptContentPtr> {
  static v8::Local<v8::Value> ToV8(
      v8::Isolate* isolate,
      const blink::mojom::AILanguageModelPromptContentPtr& val) {
    if (val.is_null())
      return v8::Undefined(isolate);

    auto dict = gin::Dictionary::CreateEmpty(isolate);

    if (val->is_text()) {
      dict.Set("type", "text");
      dict.Set("text", val->get_text());
    } else if (val->is_bitmap()) {
      // Convert the bitmap to an ArrayBuffer
      // TODO - Are we going to make any guarantees about the shape of the image
      // data?
      SkBitmap& bitmap = val->get_bitmap();

      const auto dst_info = SkImageInfo::MakeN32Premul(bitmap.dimensions());
      const size_t dst_n_bytes = dst_info.computeMinByteSize();
      auto dst_buf = v8::ArrayBuffer::New(isolate, dst_n_bytes);

      if (!bitmap.readPixels(dst_info, dst_buf->Data(), dst_info.minRowBytes(),
                             0, 0)) {
        // TODO - Handle error
      }

      dict.Set("type", "image");
      dict.Set("image", dst_buf);
    } else if (val->is_audio()) {
      // Convert the audio data to an ArrayBuffer
      // TODO - Are we going to make any guarantees about the shape of the audio
      // data?
      on_device_model::mojom::AudioDataPtr& audio_data = val->get_audio();
      std::vector<float>& raw_data = audio_data->data;

      const size_t dst_n_bytes =
          sizeof(std::remove_reference_t<decltype(raw_data)>::value_type) *
          raw_data.size();
      auto dst_buf = v8::ArrayBuffer::New(isolate, dst_n_bytes);

      UNSAFE_BUFFERS(
          std::ranges::copy(raw_data, static_cast<char*>(dst_buf->Data())));

      dict.Set("type", "audio");
      dict.Set("audio", dst_buf);
    }

    return ConvertToV8(isolate, dict);
  }
};

template <>
struct Converter<blink::mojom::AILanguageModelPromptPtr> {
  static v8::Local<v8::Value> ToV8(
      v8::Isolate* isolate,
      const blink::mojom::AILanguageModelPromptPtr& val) {
    if (val.is_null())
      return v8::Undefined(isolate);

    auto dict = gin::Dictionary::CreateEmpty(isolate);

    dict.Set("role", val->role);
    dict.Set("content", val->content);
    dict.Set("prefix", val->is_prefix);

    return ConvertToV8(isolate, dict);
  }
};

}  // namespace gin

namespace electron {

UtilityAILanguageModel::UtilityAILanguageModel(
    v8::Local<v8::Object> language_model) {
  v8::Isolate* isolate = JavascriptEnvironment::GetIsolate();
  language_model_.Reset(isolate, language_model);
}

UtilityAILanguageModel::~UtilityAILanguageModel() = default;

blink::mojom::ModelStreamingResponder* UtilityAILanguageModel::GetResponder(
    mojo::RemoteSetElementId responder_id) {
  return responder_set_.Get(responder_id);
}

blink::mojom::AIManagerCreateLanguageModelClient*
UtilityAILanguageModel::GetCreateLanguageModelClient(
    mojo::RemoteSetElementId responder_id) {
  return create_model_client_set_.Get(responder_id);
}

void UtilityAILanguageModel::Prompt(
    std::vector<blink::mojom::AILanguageModelPromptPtr> prompts,
    on_device_model::mojom::ResponseConstraintPtr constraint,
    mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
        pending_responder) {
  if (is_destroyed_) {
    mojo::Remote<blink::mojom::ModelStreamingResponder> responder(
        std::move(pending_responder));
    responder->OnError(
        blink::mojom::ModelStreamingResponseStatus::kErrorSessionDestroyed,
        /*quota_error_info=*/nullptr);
    return;
  }

  mojo::RemoteSetElementId responder_id =
      responder_set_.Add(std::move(pending_responder));

  // TODO - Add v8::TryCatch?
  v8::Isolate* isolate = JavascriptEnvironment::GetIsolate();
  v8::HandleScope scope{isolate};

  v8::Local<v8::Value> val = gin_helper::CallMethod(
      isolate, language_model_.Get(isolate), "prompt", prompts, constraint);

  auto SendResponse = [](base::WeakPtr<UtilityAILanguageModel> weak_ptr,
                         v8::Isolate* isolate,
                         mojo::RemoteSetElementId responder_id,
                         v8::Local<v8::Value> result) {
    blink::mojom::ModelStreamingResponder* responder =
        weak_ptr->GetResponder(responder_id);
    if (!responder) {
      return;
    }

    std::string response;

    // TODO - Handle the case where the return value is an instance of `ReadableStream`
    if (result->IsString() && gin::ConvertFromV8(isolate, result, &response)) {
      responder->OnStreaming(response);
      // TODO - Pull real tokens count - need to worry about parallel
      // prompts?
      responder->OnCompletion(blink::mojom::ModelExecutionContextInfo::New(0));
      return;
    } else {
      // TODO - Error handling
      responder->OnError(
          blink::mojom::ModelStreamingResponseStatus::kErrorUnknown,
          /*quota_error_info=*/nullptr);
    }
  };

  if (val->IsPromise()) {
    auto promise = val.As<v8::Promise>();

    auto then_cb = base::BindOnce(SendResponse, weak_ptr_factory_.GetWeakPtr(),
                                  isolate, responder_id);

    auto catch_cb = base::BindOnce(
        [](base::WeakPtr<UtilityAILanguageModel> weak_ptr,
           mojo::RemoteSetElementId responder_id, v8::Local<v8::Value> result) {
          // TODO - Error is here
          // TODO - An error here is killing the utility process

          blink::mojom::ModelStreamingResponder* responder =
              weak_ptr->GetResponder(responder_id);
          if (!responder) {
            return;
          }

          responder->OnError(
              blink::mojom::ModelStreamingResponseStatus::kErrorUnknown,
              /*quota_error_info=*/nullptr);
        },
        weak_ptr_factory_.GetWeakPtr(), responder_id);

    std::ignore = promise->Then(
        isolate->GetCurrentContext(),
        gin::ConvertToV8(isolate, std::move(then_cb)).As<v8::Function>(),
        gin::ConvertToV8(isolate, std::move(catch_cb)).As<v8::Function>());
  } else {
    // The method is supposed to return a promise, but for
    // convenience allow developers to return a value directly
    SendResponse(weak_ptr_factory_.GetWeakPtr(), isolate, responder_id, val);
  }
}

void UtilityAILanguageModel::Append(
    std::vector<blink::mojom::AILanguageModelPromptPtr> prompts,
    mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
        pending_responder) {
  if (is_destroyed_) {
    mojo::Remote<blink::mojom::ModelStreamingResponder> responder(
        std::move(pending_responder));
    responder->OnError(
        blink::mojom::ModelStreamingResponseStatus::kErrorSessionDestroyed,
        /*quota_error_info=*/nullptr);
    return;
  }

  mojo::RemoteSetElementId responder_id =
      responder_set_.Add(std::move(pending_responder));

  // TODO - Add v8::TryCatch?
  v8::Isolate* isolate = JavascriptEnvironment::GetIsolate();
  v8::HandleScope scope{isolate};

  v8::Local<v8::Value> val = gin_helper::CallMethod(
      isolate, language_model_.Get(isolate), "append", prompts);

  auto SendResponse = [](base::WeakPtr<UtilityAILanguageModel> weak_ptr,
                         v8::Isolate* isolate,
                         mojo::RemoteSetElementId responder_id,
                         v8::Local<v8::Value> result) {
    blink::mojom::ModelStreamingResponder* responder =
        weak_ptr->GetResponder(responder_id);
    if (!responder) {
      return;
    }

    // TODO - Confirm result is undefined, otherwise error for developer

    // TODO - Pull real tokens count - need to worry about parallel
    // prompts?
    responder->OnCompletion(blink::mojom::ModelExecutionContextInfo::New(0));
  };

  if (val->IsPromise()) {
    auto promise = val.As<v8::Promise>();

    auto then_cb = base::BindOnce(SendResponse, weak_ptr_factory_.GetWeakPtr(),
                                  isolate, responder_id);

    auto catch_cb = base::BindOnce(
        [](base::WeakPtr<UtilityAILanguageModel> weak_ptr,
           mojo::RemoteSetElementId responder_id, v8::Local<v8::Value> result) {
          // TODO - Error is here
          // TODO - An error here is killing the utility process

          blink::mojom::ModelStreamingResponder* responder =
              weak_ptr->GetResponder(responder_id);
          if (!responder) {
            return;
          }

          responder->OnError(
              blink::mojom::ModelStreamingResponseStatus::kErrorUnknown,
              /*quota_error_info=*/nullptr);
        },
        weak_ptr_factory_.GetWeakPtr(), responder_id);

    std::ignore = promise->Then(
        isolate->GetCurrentContext(),
        gin::ConvertToV8(isolate, std::move(then_cb)).As<v8::Function>(),
        gin::ConvertToV8(isolate, std::move(catch_cb)).As<v8::Function>());
  } else {
    // The method is supposed to return a promise, but for
    // convenience allow developers to return a value directly
    SendResponse(weak_ptr_factory_.GetWeakPtr(), isolate, responder_id, val);
  }
}

void UtilityAILanguageModel::Fork(
    mojo::PendingRemote<blink::mojom::AIManagerCreateLanguageModelClient>
        client) {
  // TODO - Implement this
  NOTIMPLEMENTED();
}

void UtilityAILanguageModel::Destroy() {
  is_destroyed_ = true;

  for (auto& responder : responder_set_) {
    responder->OnError(
        blink::mojom::ModelStreamingResponseStatus::kErrorSessionDestroyed,
        /*quota_error_info=*/nullptr);
  }
  responder_set_.Clear();

  v8::Isolate* isolate = JavascriptEnvironment::GetIsolate();
  v8::HandleScope scope{isolate};

  gin_helper::CallMethod(isolate, language_model_.Get(isolate), "destroy");
}

void UtilityAILanguageModel::MeasureInputUsage(
    std::vector<blink::mojom::AILanguageModelPromptPtr> input,
    MeasureInputUsageCallback callback) {
  // TODO - Add v8::TryCatch? Otherwise an error calling the method kills
  // the utility process
  v8::Isolate* isolate = JavascriptEnvironment::GetIsolate();
  v8::HandleScope scope{isolate};

  v8::Local<v8::Value> val = gin_helper::CallMethod(
      isolate, language_model_.Get(isolate), "measureInputUsage", input);

  auto RunCallback = [](v8::Isolate* isolate,
                        MeasureInputUsageCallback callback,
                        v8::Local<v8::Value> result) {
    uint32_t input_tokens = 0;

    if (result->IsNumber() &&
        gin::ConvertFromV8(isolate, result, &input_tokens)) {
      std::move(callback).Run(std::move(input_tokens));
    } else if (result->IsNull()) {
      std::move(callback).Run(std::nullopt);
    } else {
      // TODO - Error is here
      std::move(callback).Run(std::nullopt);
    }
  };

  if (val->IsPromise()) {
    auto promise = val.As<v8::Promise>();
    auto split_callback = base::SplitOnceCallback(std::move(callback));

    auto then_cb =
        base::BindOnce(RunCallback, isolate, std::move(split_callback.first));

    auto catch_cb = base::BindOnce(
        [](MeasureInputUsageCallback callback, v8::Local<v8::Value> result) {
          // TODO - Error is here
          // TODO - Need to handle the promise rejection
          std::move(callback).Run(std::nullopt);
        },
        std::move(split_callback.second));

    std::ignore = promise->Then(
        isolate->GetCurrentContext(),
        gin::ConvertToV8(isolate, std::move(then_cb)).As<v8::Function>(),
        gin::ConvertToV8(isolate, std::move(catch_cb)).As<v8::Function>());
  } else {
    // The method is supposed to return a promise, but for
    // convenience allow developers to return a value directly
    RunCallback(isolate, std::move(callback), val);
  }
}

}  // namespace electron
