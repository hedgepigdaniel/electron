// Copyright (c) 2019 Slack Technologies, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/api/atom_api_url_loader.h"

#include "base/containers/id_map.h"
#include "gin/handle.h"
#include "gin/object_template_builder.h"
#include "gin/wrappable.h"
#include "mojo/public/cpp/system/data_pipe_producer.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/chunked_data_pipe_getter.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "shell/browser/api/atom_api_session.h"
#include "shell/browser/atom_browser_context.h"
#include "shell/common/gin_converters/callback_converter.h"
#include "shell/common/gin_converters/gurl_converter.h"
#include "shell/common/gin_converters/net_converter.h"
#include "shell/common/gin_helper/dictionary.h"
#include "shell/common/gin_helper/object_template_builder.h"
#include "shell/common/node_includes.h"

class BufferDataSource : public mojo::DataPipeProducer::DataSource {
 public:
  explicit BufferDataSource(base::span<char> buffer) {
    buffer_.resize(buffer.size());
    memcpy(buffer_.data(), buffer.data(), buffer_.size());
  }
  ~BufferDataSource() override = default;

 private:
  // mojo::DataPipeProducer::DataSource:
  uint64_t GetLength() const override { return buffer_.size(); }
  ReadResult Read(uint64_t offset, base::span<char> buffer) override {
    ReadResult result;
    if (offset <= buffer_.size()) {
      size_t readable_size = buffer_.size() - offset;
      size_t writable_size = buffer.size();
      size_t copyable_size = std::min(readable_size, writable_size);
      memcpy(buffer.data(), &buffer_[offset], copyable_size);
      result.bytes_read = copyable_size;
    } else {
      NOTREACHED();
      result.result = MOJO_RESULT_OUT_OF_RANGE;
    }
    return result;
  }

  std::vector<char> buffer_;
};

class JSChunkedDataPipeGetter : public gin::Wrappable<JSChunkedDataPipeGetter>,
                                public network::mojom::ChunkedDataPipeGetter {
 public:
  static gin::Handle<JSChunkedDataPipeGetter> Create(
      v8::Isolate* isolate,
      v8::Local<v8::Function> body_func,
      mojo::PendingReceiver<network::mojom::ChunkedDataPipeGetter>
          chunked_data_pipe_getter) {
    return gin::CreateHandle(
        isolate, new JSChunkedDataPipeGetter(
                     isolate, body_func, std::move(chunked_data_pipe_getter)));
  }

  // gin::Wrappable
  gin::ObjectTemplateBuilder GetObjectTemplateBuilder(
      v8::Isolate* isolate) override {
    return gin::Wrappable<JSChunkedDataPipeGetter>::GetObjectTemplateBuilder(
               isolate)
        .SetMethod("write", &JSChunkedDataPipeGetter::WriteChunk)
        .SetMethod("done", &JSChunkedDataPipeGetter::Done);
  }

  static gin::WrapperInfo kWrapperInfo;
  ~JSChunkedDataPipeGetter() override = default;

 private:
  JSChunkedDataPipeGetter(
      v8::Isolate* isolate,
      v8::Local<v8::Function> body_func,
      mojo::PendingReceiver<network::mojom::ChunkedDataPipeGetter>
          chunked_data_pipe_getter)
      : isolate_(isolate), body_func_(isolate, body_func) {
    receiver_.Bind(std::move(chunked_data_pipe_getter));
  }

  // network::mojom::ChunkedDataPipeGetter:
  void GetSize(GetSizeCallback callback) override {
    size_callback_ = std::move(callback);
  }

  void StartReading(mojo::ScopedDataPipeProducerHandle pipe) override {
    data_producer_ = std::make_unique<mojo::DataPipeProducer>(std::move(pipe));
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

    v8::HandleScope handle_scope(isolate_);
    v8::MicrotasksScope script_scope(isolate_,
                                     v8::MicrotasksScope::kRunMicrotasks);
    auto maybe_wrapper = GetWrapper(isolate_);
    v8::Local<v8::Value> wrapper;
    if (!maybe_wrapper.ToLocal(&wrapper)) {
      return;
      // hopefully this just drops the pipe and we're good?
    }
    v8::Local<v8::Value> argv[] = {wrapper};
    node::Environment* env = node::Environment::GetCurrent(isolate_);
    auto global = env->context()->Global();
    node::MakeCallback(isolate_, global, body_func_.Get(isolate_),
                       node::arraysize(argv), argv, {0, 0});
  }

  v8::Local<v8::Promise> WriteChunk(v8::Local<v8::Value> buffer_val) {
    gin_helper::Promise<void> promise(isolate_);
    v8::Local<v8::Promise> handle = promise.GetHandle();
    if (!buffer_val->IsArrayBufferView()) {
      promise.RejectWithErrorMessage("Expected an ArrayBufferView");
      return handle;
    }
    if (is_writing_) {
      promise.RejectWithErrorMessage("Only one write can be pending at a time");
      return handle;
    }
    if (!size_callback_) {
      promise.RejectWithErrorMessage("Can't write after calling done()");
      return handle;
    }
    auto buffer = buffer_val.As<v8::ArrayBufferView>();
    is_writing_ = true;
    bytes_written_ += buffer->ByteLength();
    auto backing_store = buffer->Buffer()->GetBackingStore();
    data_producer_->Write(
        std::make_unique<BufferDataSource>(base::make_span(
            static_cast<char*>(backing_store->Data()) + buffer->ByteOffset(),
            buffer->ByteLength())),
        base::BindOnce(
            &JSChunkedDataPipeGetter::OnWriteChunkComplete,
            base::Unretained(
                this),  // TODO: should this be a weak ptr? what do we do with
                        // the promise if |this| goes away?
            std::move(promise)));
    return handle;
  }

  void OnWriteChunkComplete(gin_helper::Promise<void> promise,
                            MojoResult result) {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    is_writing_ = false;
    if (result == MOJO_RESULT_OK) {
      promise.Resolve();
    } else {
      promise.RejectWithErrorMessage("mojo result not ok");
      size_callback_.Reset();
      // ... delete this?
    }
  }

  void Done() {  // TODO: accept net error?
    if (size_callback_) {
      std::move(size_callback_).Run(net::OK, bytes_written_);
      // ... delete this?
    }
  }

  GetSizeCallback size_callback_;
  mojo::Receiver<network::mojom::ChunkedDataPipeGetter> receiver_{this};
  std::unique_ptr<mojo::DataPipeProducer> data_producer_;
  bool is_writing_ = false;
  uint64_t bytes_written_ = 0;

  v8::Isolate* isolate_;
  v8::Global<v8::Function> body_func_;
};

gin::WrapperInfo JSChunkedDataPipeGetter::kWrapperInfo = {
    gin::kEmbedderNativeGin};

namespace electron {

namespace api {

namespace {

const net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("electron_net_module", R"(
        semantics {
          sender: "Electron Net module"
          description:
            "Issue HTTP/HTTPS requests using Chromium's native networking "
            "library."
          trigger: "Using the Net module"
          data: "Anything the user wants to send."
          destination: OTHER
        }
        policy {
          cookies_allowed: YES
          cookies_store: "user"
          setting: "This feature cannot be disabled."
        })");

base::IDMap<SimpleURLLoaderWrapper*>& GetAllRequests() {
  static base::NoDestructor<base::IDMap<SimpleURLLoaderWrapper*>>
      s_all_requests;
  return *s_all_requests;
}

}  // namespace

SimpleURLLoaderWrapper::SimpleURLLoaderWrapper(
    std::unique_ptr<network::ResourceRequest> request,
    network::mojom::URLLoaderFactory* url_loader_factory)
    : id_(GetAllRequests().Add(this)) {
  // We slightly abuse the |render_frame_id| field in ResourceRequest so that
  // we can correlate any authentication events that arrive with this request.
  request->render_frame_id = id_;

  // SimpleURLLoader wants to control the request body itself. We have other
  // ideas.
  auto request_body = std::move(request->request_body);
  auto request_ref = request.get();
  loader_ =
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation);
  if (request_body) {
    request_ref->request_body = std::move(request_body);
  }

  loader_->SetOnResponseStartedCallback(base::BindOnce(
      &SimpleURLLoaderWrapper::OnResponseStarted, base::Unretained(this)));
  loader_->DownloadAsStream(url_loader_factory, this);
  /*
  loader_->SetOnRedirectCallback(
      const OnRedirectCallback& on_redirect_callback) = 0;
  loader_->SetOnUploadProgressCallback(
      UploadProgressCallback on_upload_progress_callback) = 0;
  loader_->SetOnDownloadProgressCallback(
      DownloadProgressCallback on_download_progress_callback) = 0;
  */

  // Prevent ourselves from being GC'd until the request is complete.
  pinned_wrapper_.Reset(isolate(), GetWrapper());
}

SimpleURLLoaderWrapper::~SimpleURLLoaderWrapper() {
  GetAllRequests().Remove(id_);
}

// static
SimpleURLLoaderWrapper* SimpleURLLoaderWrapper::FromID(uint32_t id) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  return GetAllRequests().Lookup(id);
}

void SimpleURLLoaderWrapper::OnAuthRequired(
    const GURL& url,
    bool first_auth_attempt,
    net::AuthChallengeInfo auth_info,
    network::mojom::URLResponseHeadPtr head,
    mojo::PendingRemote<network::mojom::AuthChallengeResponder>
        auth_challenge_responder) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  mojo::Remote<network::mojom::AuthChallengeResponder> auth_responder(
      std::move(auth_challenge_responder));
  // auth_responder.set_disconnect_handler(
  //    base::BindOnce(&SimpleURLLoaderWrapper::Cancel,
  //    weak_factory_.GetWeakPtr()));
  auto cb = base::BindOnce(
      [](mojo::Remote<network::mojom::AuthChallengeResponder> auth_responder,
         gin::Arguments* args) {
        base::string16 username_str, password_str;
        if (!args->GetNext(&username_str) || !args->GetNext(&password_str)) {
          auth_responder->OnAuthCredentials(base::nullopt);
          return;
        }
        auth_responder->OnAuthCredentials(
            net::AuthCredentials(username_str, password_str));
      },
      std::move(auth_responder));
  Emit("login", auth_info, base::AdaptCallbackForRepeating(std::move(cb)));
}

void SimpleURLLoaderWrapper::Cancel() {
  loader_.reset();
  pinned_wrapper_.Reset();
  // This ensures that no further callbacks will be called, so there's no need
  // for additional guards.
}

// static
mate::WrappableBase* SimpleURLLoaderWrapper::New(gin::Arguments* args) {
  gin_helper::Dictionary opts;
  if (!args->GetNext(&opts)) {
    args->ThrowTypeError("Expected a dictionary");
    return nullptr;
  }
  auto request = std::make_unique<network::ResourceRequest>();
  opts.Get("method", &request->method);
  opts.Get("url", &request->url);
  std::map<std::string, std::string> extra_headers;
  if (opts.Get("extraHeaders", &extra_headers)) {
    for (const auto& it : extra_headers) {
      if (net::HttpUtil::IsValidHeaderName(it.first) &&
          net::HttpUtil::IsValidHeaderValue(it.second)) {
        request->headers.SetHeader(it.first, it.second);
      } else {
        // TODO: warning...? or, better, warning when the user calls
        // setHeader...
      }
    }
  }
  opts.Get("redirect", &request->redirect_mode);

  v8::Local<v8::Value> body;
  if (opts.Get("body", &body)) {
    if (body->IsArrayBufferView()) {
      auto buffer_body = body.As<v8::ArrayBufferView>();
      auto backing_store = buffer_body->Buffer()->GetBackingStore();
      request->request_body = network::ResourceRequestBody::CreateFromBytes(
          static_cast<char*>(backing_store->Data()) + buffer_body->ByteOffset(),
          buffer_body->ByteLength());
    } else if (body->IsFunction()) {
      auto body_func = body.As<v8::Function>();

      mojo::PendingRemote<network::mojom::ChunkedDataPipeGetter>
          data_pipe_getter;
      JSChunkedDataPipeGetter::Create(
          args->isolate(), body_func,
          data_pipe_getter.InitWithNewPipeAndPassReceiver());
      request->request_body = new network::ResourceRequestBody();
      request->request_body->SetToChunkedDataPipe(std::move(data_pipe_getter));
    }
  }

  std::string partition;
  gin::Handle<Session> session;
  if (!opts.Get("session", &session)) {
    if (opts.Get("partition", &partition))
      session = Session::FromPartition(args->isolate(), partition);
    else  // default session
      session = Session::FromPartition(args->isolate(), "");
  }

  auto url_loader_factory = session->browser_context()->GetURLLoaderFactory();

  auto* ret =
      new SimpleURLLoaderWrapper(std::move(request), url_loader_factory.get());
  ret->InitWithArgs(args);
  return ret;
}

void SimpleURLLoaderWrapper::OnDataReceived(base::StringPiece string_piece,
                                            base::OnceClosure resume) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  v8::HandleScope handle_scope(isolate());
  auto array_buffer = v8::ArrayBuffer::New(isolate(), string_piece.size());
  auto backing_store = array_buffer->GetBackingStore();
  memcpy(backing_store->Data(), string_piece.data(), string_piece.size());
  Emit("data", array_buffer);
  std::move(resume).Run();
}

void SimpleURLLoaderWrapper::OnComplete(bool success) {
  if (success) {
    Emit("complete");
  } else {
    Emit("error", net::ErrorToString(loader_->NetError()));
  }
  loader_.reset();
  pinned_wrapper_.Reset();
}

void SimpleURLLoaderWrapper::OnRetry(base::OnceClosure start_retry) {}

void SimpleURLLoaderWrapper::OnResponseStarted(
    const GURL& final_url,
    const network::mojom::URLResponseHead& response_head) {
  gin::Dictionary dict = gin::Dictionary::CreateEmpty(isolate());
  dict.Set("statusCode", response_head.headers->response_code());
  Emit("response-started", final_url, dict);
}

// static
void SimpleURLLoaderWrapper::BuildPrototype(
    v8::Isolate* isolate,
    v8::Local<v8::FunctionTemplate> prototype) {
  prototype->SetClassName(gin::StringToV8(isolate, "SimpleURLLoaderWrapper"));
  gin_helper::ObjectTemplateBuilder(isolate, prototype->PrototypeTemplate())
      .SetMethod("cancel", &SimpleURLLoaderWrapper::Cancel);
}

}  // namespace api

}  // namespace electron