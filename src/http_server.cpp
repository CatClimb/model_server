//*****************************************************************************
// Copyright 2020 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************
#include "http_server.hpp"
#include <iostream>
#include <memory>
#include <regex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "tensorflow_serving/util/net_http/public/response_code_enum.h"
#include "tensorflow_serving/util/net_http/server/public/httpserver.h"
#include "tensorflow_serving/util/net_http/server/public/server_request_interface.h"
#include "tensorflow_serving/util/threadpool_executor.h"
#pragma GCC diagnostic pop

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include "http_rest_api_handler.hpp"
namespace ovms {

namespace net_http2 = ovms;
class ThreadPool { /* 使用之前的 NativeThreadPool 实现 */
};

class RestApiRequestDispatcher {
public:
    RestApiRequestDispatcher(ovms::Server& ovmsServer, int timeout_in_ms) {
        handler_ = std::make_unique<HttpRestApiHandler>(ovmsServer, timeout_in_ms);
    }

    HttpResponse dispatch(net_http2::HttpRequest* req) {
        // return [this](net_http2::HttpRequest* req) {
        //     try {
        //         this->processRequest(req);
        //     } catch (...) {
        //         SPDLOG_DEBUG("Exception caught in REST request handler");
        //         // req->ReplyWithStatus(HTTPStatusCode2::ERROR);
        //     }
        // };
        try {
            // 这里可能抛出任何类型的异常
            return this->processRequest(req);
        } catch (...) {
            std::exception_ptr eptr = std::current_exception();
            try {
                if (eptr) {
                    std::rethrow_exception(eptr);
                }
            } catch (const std::exception& e) {
                std::cout << "Caught exception: " << e.what() << std::endl;
            } catch (...) {
                std::cout << "Caught an unknown exception." << std::endl;
            }
        }
        
            
    };

private:
    void parseHeaders(net_http2::HttpRequest* req, std::vector<std::pair<std::string, std::string>>* headers) {

        if (req->get_header_value("Inference-Header-Content-Length").size() > 0) {
            std::pair<std::string, std::string> header{"Inference-Header-Content-Length", req->get_header_value("Inference-Header-Content-Length")};
            headers->emplace_back(header);
        }
    }
    HttpResponse processRequest(net_http2::HttpRequest* req) {
        net_http2::HttpResponse res;
        SPDLOG_DEBUG("REST request {}", req->path);
        std::string body;
        // int64_t num_bytes = 0;
        // auto request_chunk = req->ReadRequestBytes(&num_bytes);
        body = req->body;
        // while (request_chunk != nullptr) {
        //     body.append(std::string_view(request_chunk.get(), num_bytes));
        //     request_chunk = req->ReadRequestBytes(&num_bytes);
        // }

        std::vector<std::pair<std::string, std::string>> headers;
        parseHeaders(req, &headers);
        std::string output;
        SPDLOG_DEBUG("Processing HTTP request: {} {} body: {} bytes",
            // req->http_method(),
            req->method,
            // req->uri_path(),
            req->path,
            body.size());
        HttpResponseComponents responseComponents;
        const auto status = handler_->processRequest(req->method, req->path, body, &headers, &output, responseComponents);
        if (!status.ok() && output.empty()) {
            output.append("{\"error\": \"" + status.string() + "\"}");
        }
        const auto http_status = http(status);
        if (http_status != HTTPStatusCode2::OK && http_status != HTTPStatusCode2::CREATED) {
                SPDLOG_DEBUG("Processing HTTP/REST request failed: {} {}. Reason: {}",
                    req->method,
                    req->path,
                    status.string());
        }
        if (responseComponents.inferenceHeaderContentLength.has_value()) {
            std::pair<std::string, std::string> header{"Inference-Header-Content-Length", std::to_string(responseComponents.inferenceHeaderContentLength.value())};
            headers.emplace_back(header);
        }
        for (const auto& kv : headers) {
            req->OverwriteResponseHeader(&res,kv.first, kv.second);
        }
        res.headers = res.ConvertVectorToMap(headers);
        std::cout <<  "预测结果：" << output <<  std::endl;

        res.body = output;
        std::cout <<  "预测结果状态：" << status.string() <<  std::endl;
        return res;
        // req->WriteResponseString(output);
        // if (http_status != HTTPStatusCode2::OK && http_status != HTTPStatusCode2::CREATED) {
        //     SPDLOG_DEBUG("Processing HTTP/REST request failed: {} {}. Reason: {}",
        //         req->method,
        //         req->path,
        //         status.string());
        // }
        // req->ReplyWithStatus(http_status);
    }

    std::unique_ptr<HttpRestApiHandler> handler_;
};

//================== HTTP 核心组件 ==================
// HTTP 响应结构体

std::unordered_map<std::string, std::string> HttpResponse::ConvertVectorToMap(
    const std::vector<std::pair<std::string, std::string>>& headers_vec) {

    std::unordered_map<std::string, std::string> headers_map;
    for (const auto& [key, value] : headers_vec) {  // C++17 结构化绑定
        headers_map.insert_or_assign(key, value);   // 避免重复构造临时对象
    }
    return headers_map;
}
// HTTP 请求结构体
std::string HttpRequest::get_header_value(const std::string& key) const {
    // 安全校验：确保req非空

    auto it = headers.find(key);  // 查找键
    if (it != headers.end()) {    // 存在则返回值
        return it->second;
    } else {  // 不存在则返回空字符串
        return "";
    }
}
void HttpRequest::OverwriteResponseHeader(HttpResponse* res, const std::string& key, const std::string& value) {
    if (!res)
        return;  // 空指针检查

    // 使用 map 的 insert 或 assign 特性覆盖值
    if (auto it = res->headers.find(key); it != res->headers.end()) {
        it->second = value;  // 存在则覆盖
    } else {
        res->headers.emplace(key, value);  // 不存在则插入
    }
}

HttpServer* HttpServer::CreateHTTPServer(const std::string& address, uint16_t port, int num_threads) {
    return new HttpServer(address, port, num_threads);
}

void HttpServer::RegisterRequestDispatcher(RestApiRequestDispatcher* dispatcher) {
    m_dispatcher = dispatcher;
}
void HttpServer::StartAcceptingRequests() {
    start_server();
}
// 在其他公共方法之后添加
void HttpServer::StopServer() {
    {
        std::lock_guard<std::mutex> lock(m_server_fd_mutex);
        if (!m_running)
            return;

        // Step 1: 设置停止标志位
        m_running = false;

        // Step 2: 关闭服务器套接字（触发accept解除阻塞）
        if (m_server_fd != -1) {
            shutdown(m_server_fd, SHUT_RDWR);
            close(m_server_fd);
            m_server_fd = -1;
        }
    }

    // Step 3: 唤醒所有工作线程
    m_cv.notify_all();

    // Step 4: 等待线程结束
    for (auto& t : m_workers) {
        if (t.joinable()) {
            t.join();
        }
    }
}
HttpServer::~HttpServer() {
    close(m_server_fd);
}

HttpServer::HttpServer(const std::string& address, uint16_t port, int num_threads) :
    m_address(address), m_port(port), m_num_threads(num_threads) {}

void HttpServer::start_server() {
    std::cout << "当前rest工作线程： " << m_num_threads << std::endl;
    // 创建 socket
    m_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_server_fd < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }
    // 配置地址
    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(m_port);
    inet_pton(AF_INET, m_address.c_str(), &server_addr.sin_addr);
    // 绑定地址
    int opt = 1;
    setsockopt(m_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(m_server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    // 开始监听
    if (listen(m_server_fd, SOMAXCONN) < 0) {
        perror("listen failed");
        exit(EXIT_FAILURE);
    }
    std::cout << "Server started on " << m_address << ":" << m_port << std::endl;
    // 创建工作者线程
    for (int i = 0; i < m_num_threads; ++i) {
        m_workers.emplace_back([this] { accept_connections(); });
    }
    // 等待所有线程完成
    for (auto& t : m_workers) {
        t.detach();
    }
}

void HttpServer::accept_connections() {
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(m_server_fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            perror("accept failed");
            continue;
        }
        std::cout << "我接收到1请求 哈哈 " << std::endl;
        handle_connection(client_fd);
    }
}

void HttpServer::handle_connection(int client_fd) {
    // char buffer[4096] = {0};
    char buffer[12288] = {0};  // 1280 字节的缓冲区
    std::string request_str;
    // ssize_t bytes_read;
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));
    // while ((bytes_read = read(client_fd, buffer, sizeof(buffer))) > 0) {
    //     request_str.append(buffer, bytes_read);
    //     // 可以在这里进行部分解析处理
    // }
    if (bytes_read > 0) {
    // if (!request_str.empty()) {
        std::cout << "请求转换前 " << std::endl;
        // HttpRequest req = parse_request(request_str);
        HttpRequest req = parse_request(std::string(buffer, bytes_read));
        std::cout << "请求转换后path " <<req.path <<std::endl;
        std::cout << "请求转换后version " <<req.version <<std::endl;
        std::cout << "调用请求适配器处理器前 " << std::endl;
        HttpResponse res =m_dispatcher->dispatch(&req);
        std::cout << "调用请求适配器处理器后 " << std::endl;
        std::cout << "Server started on发送前 " << std::endl;
        
        std::string response_str = build_response(res);
        std::cout << "打印下结果 哈哈： "<<response_str << std::endl;
        send(client_fd, response_str.c_str(), response_str.size(), 0);
        std::cout << "Server started on发送后 " << std::endl;
    }

    close(client_fd);
}

HttpRequest HttpServer::parse_request(const std::string& raw_request) {
    HttpRequest req;
    std::istringstream stream(raw_request);
    std::string line;

    // 解析请求行
    if (std::getline(stream, line)) {
        std::istringstream line_stream(line);
        line_stream >> req.method >> req.path >> req.version;
    }

    // 处理转义字符
    req.path = url_decode(req.path);

    // 解析请求头
    while (std::getline(stream, line) && line != "\r") {
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 2, line.size() - colon_pos - 3);
            req.headers[key] = value;
        }
    }

    // 解析请求体（简化处理）
    size_t body_pos = raw_request.find("\r\n\r\n");
    if (body_pos != std::string::npos) {
        req.body = raw_request.substr(body_pos + 4);
    }

    return req;
}

std::string HttpServer::url_decode(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '%') {
            if (i + 2 < str.size()) {
                int value;
                std::istringstream hex_stream(str.substr(i + 1, 2));
                if (hex_stream >> std::hex >> value) {
                    result += static_cast<char>(value);
                    i += 2;
                }
            }
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    return result;
}

std::string HttpServer::build_response(const HttpResponse& res) {
    std::ostringstream stream;
    stream << res.version << " "
           << res.status_code << " "
           << res.status_text << "\r\n";

    for (const auto& [key, value] : res.headers) {
        stream << key << ": " << value << "\r\n";
    }

    stream << "Content-Length: " << res.body.size() << "\r\n\r\n";
    stream << res.body;
    return stream.str();
}

//============= 使用示例 =============
// int main() {
//     auto server = HttpServer::CreateHTTPServer("192.168.3.101", 8080, 4);

//     // 注册路由处理器
//     server->GetRouter().Register("GET", "/",
//         [](const HttpRequest&, HttpResponse& res) {
//             res.content = "<h1>Welcome to Native C++ Server</h1>";
//         });

//     server->GetRouter().Register("GET", "/hello",
//         [](const HttpRequest&, HttpResponse& res) {
//             res.content = "<h1>Hello from C++17</h1>";
//         });

//     server->Start();

//     // 等待退出（实际应用中可以添加信号处理）
//     while(true) std::this_thread::sleep_for(std::chrono::seconds(1));

//     server->Stop();
//     return 0;
// }

// 构造函数，用于初始化状态码
// Status::Status(StatusCode c) :
//     code(c) {}

// // 获取状态码的方法
// StatusCode Status::getCode() const {
//     return code;
// }

const HTTPStatusCode2 http(const ovms::Status& status) {
    const std::unordered_map<const StatusCode, HTTPStatusCode2> httpStatusMap = {
        {StatusCode::OK, HTTPStatusCode2::OK},
        {StatusCode::OK_RELOADED, HTTPStatusCode2::CREATED},
        {StatusCode::OK_NOT_RELOADED, HTTPStatusCode2::OK},

        // REST handler failure
        {StatusCode::REST_INVALID_URL, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_UNSUPPORTED_METHOD, HTTPStatusCode2::NONE_ACC},
        {StatusCode::REST_NOT_FOUND, HTTPStatusCode2::NOT_FOUND},

        // REST parser failure
        {StatusCode::REST_BODY_IS_NOT_AN_OBJECT, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_PREDICT_UNKNOWN_ORDER, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_INSTANCES_NOT_AN_ARRAY, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_NAMED_INSTANCE_NOT_AN_OBJECT, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_INPUT_NOT_PREALLOCATED, HTTPStatusCode2::ERROR},
        {StatusCode::REST_NO_INSTANCES_FOUND, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_INSTANCES_NOT_NAMED_OR_NONAMED, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_COULD_NOT_PARSE_INSTANCE, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_INSTANCES_BATCH_SIZE_DIFFER, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_INPUTS_NOT_AN_OBJECT, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_NO_INPUTS_FOUND, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_COULD_NOT_PARSE_INPUT, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_COULD_NOT_PARSE_OUTPUT, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_COULD_NOT_PARSE_PARAMETERS, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_PROTO_TO_STRING_ERROR, HTTPStatusCode2::ERROR},
        {StatusCode::REST_UNSUPPORTED_PRECISION, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_SERIALIZE_TENSOR_CONTENT_INVALID_SIZE, HTTPStatusCode2::ERROR},
        {StatusCode::REST_BINARY_BUFFER_EXCEEDED, HTTPStatusCode2::BAD_REQUEST},

        {StatusCode::PATH_INVALID, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::FILE_INVALID, HTTPStatusCode2::ERROR},
        {StatusCode::NO_MODEL_VERSION_AVAILABLE, HTTPStatusCode2::ERROR},
        {StatusCode::MODEL_NOT_LOADED, HTTPStatusCode2::ERROR},
        {StatusCode::JSON_INVALID, HTTPStatusCode2::PRECOND_FAILED},
        {StatusCode::MODELINSTANCE_NOT_FOUND, HTTPStatusCode2::ERROR},
        {StatusCode::SHAPE_WRONG_FORMAT, HTTPStatusCode2::ERROR},
        {StatusCode::PLUGIN_CONFIG_WRONG_FORMAT, HTTPStatusCode2::ERROR},
        {StatusCode::MODEL_VERSION_POLICY_WRONG_FORMAT, HTTPStatusCode2::ERROR},
        {StatusCode::MODEL_VERSION_POLICY_UNSUPPORTED_KEY, HTTPStatusCode2::ERROR},
        {StatusCode::RESHAPE_ERROR, HTTPStatusCode2::PRECOND_FAILED},
        {StatusCode::MODEL_MISSING, HTTPStatusCode2::NOT_FOUND},
        {StatusCode::MODEL_NAME_MISSING, HTTPStatusCode2::NOT_FOUND},
        {StatusCode::PIPELINE_DEFINITION_NAME_MISSING, HTTPStatusCode2::NOT_FOUND},
        {StatusCode::MEDIAPIPE_DEFINITION_NAME_MISSING, HTTPStatusCode2::NOT_FOUND},
        {StatusCode::MODEL_VERSION_MISSING, HTTPStatusCode2::NOT_FOUND},
        {StatusCode::MODEL_VERSION_NOT_LOADED_ANYMORE, HTTPStatusCode2::NOT_FOUND},
        {StatusCode::MODEL_VERSION_NOT_LOADED_YET, HTTPStatusCode2::SERVICE_UNAV},
        {StatusCode::PIPELINE_DEFINITION_NOT_LOADED_YET, HTTPStatusCode2::SERVICE_UNAV},
        {StatusCode::PIPELINE_DEFINITION_NOT_LOADED_ANYMORE, HTTPStatusCode2::NOT_FOUND},
        {StatusCode::MODEL_SPEC_MISSING, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_SIGNATURE_DEF, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::PIPELINE_DEMULTIPLEXER_NO_RESULTS, HTTPStatusCode2::NO_CONTENT},
        {StatusCode::CANNOT_COMPILE_MODEL_INTO_TARGET_DEVICE, HTTPStatusCode2::PRECOND_FAILED},

        // Sequence management
        {StatusCode::SEQUENCE_MISSING, HTTPStatusCode2::NOT_FOUND},
        {StatusCode::SEQUENCE_ALREADY_EXISTS, HTTPStatusCode2::CONFLICT},
        {StatusCode::SEQUENCE_ID_NOT_PROVIDED, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_SEQUENCE_CONTROL_INPUT, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::SEQUENCE_ID_BAD_TYPE, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::SEQUENCE_CONTROL_INPUT_BAD_TYPE, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::SEQUENCE_TERMINATED, HTTPStatusCode2::PRECOND_FAILED},
        {StatusCode::SPECIAL_INPUT_NO_TENSOR_SHAPE, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::MAX_SEQUENCE_NUMBER_REACHED, HTTPStatusCode2::SERVICE_UNAV},

        // Predict request validation
        {StatusCode::INVALID_NO_OF_INPUTS, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_MISSING_INPUT, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_UNEXPECTED_INPUT, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_NO_OF_SHAPE_DIMENSIONS, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_BATCH_SIZE, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_SHAPE, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_BUFFER_TYPE, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_DEVICE_ID, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_STRING_INPUT, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_INPUT_FORMAT, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_PRECISION, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_VALUE_COUNT, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_CONTENT_SIZE, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_MESSAGE_STRUCTURE, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::UNSUPPORTED_LAYOUT, HTTPStatusCode2::BAD_REQUEST},

        // Deserialization

        // Should never occur - ModelInstance::validate takes care of that
        {StatusCode::OV_UNSUPPORTED_DESERIALIZATION_PRECISION, HTTPStatusCode2::ERROR},
        {StatusCode::OV_INTERNAL_DESERIALIZATION_ERROR, HTTPStatusCode2::ERROR},

        // Inference
        {StatusCode::OV_INTERNAL_INFERENCE_ERROR, HTTPStatusCode2::ERROR},

        // Serialization

        // Should never occur - it should be validated during model loading
        {StatusCode::OV_UNSUPPORTED_SERIALIZATION_PRECISION, HTTPStatusCode2::ERROR},
        {StatusCode::OV_INTERNAL_SERIALIZATION_ERROR, HTTPStatusCode2::ERROR},

        // GetModelStatus
        {StatusCode::INTERNAL_ERROR, HTTPStatusCode2::ERROR},

        // Binary input
        {StatusCode::INVALID_NO_OF_CHANNELS, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::BINARY_IMAGES_RESOLUTION_MISMATCH, HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::STRING_VAL_EMPTY, HTTPStatusCode2::BAD_REQUEST},
    };
    auto it = httpStatusMap.find(status.getCode());
    if (it != httpStatusMap.end()) {
        return it->second;
    } else {
        return HTTPStatusCode2::ERROR;
    }
}

// class RequestExecutor final : public net_http::EventExecutor {
// public:
//     explicit RequestExecutor(int num_threads) :
//         executor_(tensorflow::Env::Default(), "httprestserver", num_threads) {}

//     void Schedule(std::function<void()> fn) override { executor_.Schedule(fn); }

// private:
//     tensorflow::serving::ThreadPoolExecutor executor_;
// };

std::unique_ptr<HttpServer> createAndStartHttpServer(const std::string& address, int port, int num_threads, ovms::Server& ovmsServer, int timeout_in_ms) {
    // auto options = std::make_unique<net_http::ServerOptions>();
    // options->AddPort(static_cast<uint32_t>(port));
    // options->SetAddress(address);
    // options->SetExecutor(std::make_unique<RequestExecutor>(num_threads));
    std::cout <<  "服务器创建前" <<  std::endl;
    auto server = HttpServer::CreateHTTPServer(address, static_cast<uint32_t>(port), num_threads);
    // auto server = net_http::CreateEvHTTPServer(std::move(options));
    if (server == nullptr) {
        SPDLOG_ERROR("Failed to create http server");
        return nullptr;
    }
    std::cout <<  "服务器创建后" <<  std::endl;
    std::cout <<  "适配器创建前" <<  std::endl;
    std::shared_ptr<RestApiRequestDispatcher> dispatcher =
        std::make_shared<RestApiRequestDispatcher>(ovmsServer, timeout_in_ms);
    std::cout <<  "适配器创建后" <<  std::endl;
    // net_http::RequestHandlerOptions handler_options;
    // server->RegisterRequestDispatcher(
    //     [dispatcher](net_http::ServerRequestInterface* req) {
    //         return dispatcher->dispatch(req);
    //     },
    //     handler_options);
    std::cout <<  "适配器注册前" <<  std::endl;
    server->RegisterRequestDispatcher(dispatcher.get());
    std::cout <<  "适配器注册后" <<  std::endl;
    std::cout <<  "http服务开启前" <<  std::endl;
    server->StartAcceptingRequests();
        SPDLOG_INFO("REST server listening on port {} with {} threads", port, num_threads);
        std::cout <<  "http服务开启后" <<  std::endl;
        return std::unique_ptr<ovms::HttpServer>(server);
    
    
    std::cout <<  "http服务开启失败后" <<  std::endl;
    //// if (server->StartAcceptingRequests()) {
    ////     SPDLOG_INFO("REST server listening on port {} with {} threads", port, num_threads);
    ////     return *server;
    //// }

    // return nullptr;
    // return nullptr;
}
}  // namespace ovms
