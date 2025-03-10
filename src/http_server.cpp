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

#include "http_rest_api_handler.hpp"
#include "status.hpp"



#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#include <atomic>
#include <mutex>



namespace ovms {

namespace net_http2 = ovms;
class ThreadPool { /* 使用之前的 NativeThreadPool 实现 */ };




//================== HTTP 核心组件 ==================

// HTTP 请求结构体
struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::string get_header_value( const std::string& key) {
         // 安全校验：确保req非空
    
        auto it =headers.find(key);       // 查找键
        if (it != headers.end()) {         // 存在则返回值
            return it->second;
        } else {                                // 不存在则返回空字符串
            return "";
        }
    }
    void OverwriteResponseHeader(HttpResponse* res, const std::string& key, const std::string& value) {
        if (!res) return; // 空指针检查
        
        // 使用 map 的 insert 或 assign 特性覆盖值
        if (auto it = res->headers.find(key); it != res->headers.end()) {
            it->second = value; // 存在则覆盖
        } else {
            res->headers.emplace(key, value); // 不存在则插入
        }
    }
};

// HTTP 响应结构体
struct HttpResponse {
    std::string version = "HTTP/1.1";
    int status_code = 200;
    std::string status_text = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    std::unordered_map<std::string, std::string> ConvertVectorToMap(
        const std::vector<std::pair<std::string, std::string>>& headers_vec) {
        
        std::unordered_map<std::string, std::string> headers_map;
        for (const auto& [key, value] : headers_vec) {  // C++17 结构化绑定
            headers_map.insert_or_assign(key, value);   // 避免重复构造临时对象
        }
        return headers_map;
    }
};

class HttpServer {
public:
    static HttpServer* CreateHTTPServer(const std::string& address, uint16_t port, int num_threads = 4) {
        return new HttpServer(address, port, num_threads);
    }

    void RegisterRequestDispatcher(RestApiRequestDispatcher* dispatcher) {
        m_dispatcher = dispatcher;
    }
    void StartAcceptingRequests() {
        start_server();
    }
    // 在其他公共方法之后添加
    void StopServer() {
        {
            std::lock_guard<std::mutex> lock(m_server_fd_mutex);
            if (!m_running) return;
            
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
    ~HttpServer() {
        close(m_server_fd);
    }

private:
    HttpServer(const std::string& address, uint16_t port, int num_threads)
        : m_address(address), m_port(port), m_num_threads(num_threads) {}

    void start_server() {
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
            t.join();
        }
    }

    void accept_connections() {
        while (true) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(m_server_fd, (struct sockaddr*)&client_addr, &client_len);
            
            if (client_fd < 0) {
                perror("accept failed");
                continue;
            }

            handle_connection(client_fd);
        }
    }

    void handle_connection(int client_fd) {
        char buffer[4096] = {0};
        ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));
        
        if (bytes_read > 0) {
            HttpRequest req = parse_request(std::string(buffer, bytes_read));
            HttpResponse res = m_dispatcher->dispatch(req);
            std::cout << "Server started on发送前 " << std::endl;

            std::string response_str = build_response(res);
            send(client_fd, response_str.c_str(), response_str.size(), 0);
            std::cout << "Server started on发送后 " << std::endl;
        }

        close(client_fd);
    }

    HttpRequest parse_request(const std::string& raw_request) {
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

    std::string url_decode(const std::string &str) {
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

    std::string build_response(const HttpResponse& res) {
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

    int m_server_fd;
    std::string m_address;
    uint16_t m_port;
    int m_num_threads;
    std::vector<std::thread> m_workers;
    RestApiRequestDispatcher* m_dispatcher;
    std::atomic<bool> m_running{false};  
    std::mutex m_server_fd_mutex;  
    std::condition_variable m_cv;      
};
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
static const net_http2::HTTPStatusCode2 http(const ovms::Status& status) {
    const std::unordered_map<const StatusCode, net_http2::HTTPStatusCode2> httpStatusMap = {
        {StatusCode::OK, net_http2::HTTPStatusCode2::OK},
        {StatusCode::OK_RELOADED, net_http2::HTTPStatusCode2::CREATED},
        {StatusCode::OK_NOT_RELOADED, net_http2::HTTPStatusCode2::OK},

        // REST handler failure
        {StatusCode::REST_INVALID_URL, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_UNSUPPORTED_METHOD, net_http2::HTTPStatusCode2::NONE_ACC},
        {StatusCode::REST_NOT_FOUND, net_http2::HTTPStatusCode2::NOT_FOUND},

        // REST parser failure
        {StatusCode::REST_BODY_IS_NOT_AN_OBJECT, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_PREDICT_UNKNOWN_ORDER, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_INSTANCES_NOT_AN_ARRAY, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_NAMED_INSTANCE_NOT_AN_OBJECT, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_INPUT_NOT_PREALLOCATED, net_http2::HTTPStatusCode2::ERROR},
        {StatusCode::REST_NO_INSTANCES_FOUND, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_INSTANCES_NOT_NAMED_OR_NONAMED, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_COULD_NOT_PARSE_INSTANCE, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_INSTANCES_BATCH_SIZE_DIFFER, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_INPUTS_NOT_AN_OBJECT, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_NO_INPUTS_FOUND, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_COULD_NOT_PARSE_INPUT, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_COULD_NOT_PARSE_OUTPUT, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_COULD_NOT_PARSE_PARAMETERS, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_PROTO_TO_STRING_ERROR, net_http2::HTTPStatusCode2::ERROR},
        {StatusCode::REST_UNSUPPORTED_PRECISION, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::REST_SERIALIZE_TENSOR_CONTENT_INVALID_SIZE, net_http2::HTTPStatusCode2::ERROR},
        {StatusCode::REST_BINARY_BUFFER_EXCEEDED, net_http2::HTTPStatusCode2::BAD_REQUEST},

        {StatusCode::PATH_INVALID, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::FILE_INVALID, net_http2::HTTPStatusCode2::ERROR},
        {StatusCode::NO_MODEL_VERSION_AVAILABLE, net_http2::HTTPStatusCode2::ERROR},
        {StatusCode::MODEL_NOT_LOADED, net_http2::HTTPStatusCode2::ERROR},
        {StatusCode::JSON_INVALID, net_http2::HTTPStatusCode2::PRECOND_FAILED},
        {StatusCode::MODELINSTANCE_NOT_FOUND, net_http2::HTTPStatusCode2::ERROR},
        {StatusCode::SHAPE_WRONG_FORMAT, net_http2::HTTPStatusCode2::ERROR},
        {StatusCode::PLUGIN_CONFIG_WRONG_FORMAT, net_http2::HTTPStatusCode2::ERROR},
        {StatusCode::MODEL_VERSION_POLICY_WRONG_FORMAT, net_http2::HTTPStatusCode2::ERROR},
        {StatusCode::MODEL_VERSION_POLICY_UNSUPPORTED_KEY, net_http2::HTTPStatusCode2::ERROR},
        {StatusCode::RESHAPE_ERROR, net_http2::HTTPStatusCode2::PRECOND_FAILED},
        {StatusCode::MODEL_MISSING, net_http2::HTTPStatusCode2::NOT_FOUND},
        {StatusCode::MODEL_NAME_MISSING, net_http2::HTTPStatusCode2::NOT_FOUND},
        {StatusCode::PIPELINE_DEFINITION_NAME_MISSING, net_http2::HTTPStatusCode2::NOT_FOUND},
        {StatusCode::MEDIAPIPE_DEFINITION_NAME_MISSING, net_http2::HTTPStatusCode2::NOT_FOUND},
        {StatusCode::MODEL_VERSION_MISSING, net_http2::HTTPStatusCode2::NOT_FOUND},
        {StatusCode::MODEL_VERSION_NOT_LOADED_ANYMORE, net_http2::HTTPStatusCode2::NOT_FOUND},
        {StatusCode::MODEL_VERSION_NOT_LOADED_YET, net_http2::HTTPStatusCode2::SERVICE_UNAV},
        {StatusCode::PIPELINE_DEFINITION_NOT_LOADED_YET, net_http2::HTTPStatusCode2::SERVICE_UNAV},
        {StatusCode::PIPELINE_DEFINITION_NOT_LOADED_ANYMORE, net_http2::HTTPStatusCode2::NOT_FOUND},
        {StatusCode::MODEL_SPEC_MISSING, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_SIGNATURE_DEF, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::PIPELINE_DEMULTIPLEXER_NO_RESULTS, net_http2::HTTPStatusCode2::NO_CONTENT},
        {StatusCode::CANNOT_COMPILE_MODEL_INTO_TARGET_DEVICE, net_http2::HTTPStatusCode2::PRECOND_FAILED},

        // Sequence management
        {StatusCode::SEQUENCE_MISSING, net_http2::HTTPStatusCode2::NOT_FOUND},
        {StatusCode::SEQUENCE_ALREADY_EXISTS, net_http2::HTTPStatusCode2::CONFLICT},
        {StatusCode::SEQUENCE_ID_NOT_PROVIDED, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_SEQUENCE_CONTROL_INPUT, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::SEQUENCE_ID_BAD_TYPE, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::SEQUENCE_CONTROL_INPUT_BAD_TYPE, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::SEQUENCE_TERMINATED, net_http2::HTTPStatusCode2::PRECOND_FAILED},
        {StatusCode::SPECIAL_INPUT_NO_TENSOR_SHAPE, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::MAX_SEQUENCE_NUMBER_REACHED, net_http2::HTTPStatusCode2::SERVICE_UNAV},

        // Predict request validation
        {StatusCode::INVALID_NO_OF_INPUTS, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_MISSING_INPUT, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_UNEXPECTED_INPUT, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_NO_OF_SHAPE_DIMENSIONS, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_BATCH_SIZE, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_SHAPE, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_BUFFER_TYPE, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_DEVICE_ID, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_STRING_INPUT, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_INPUT_FORMAT, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_PRECISION, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_VALUE_COUNT, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_CONTENT_SIZE, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::INVALID_MESSAGE_STRUCTURE, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::UNSUPPORTED_LAYOUT, net_http2::HTTPStatusCode2::BAD_REQUEST},

        // Deserialization
f
        // Should never occur - ModelInstance::validate takes care of that
        {StatusCode::OV_UNSUPPORTED_DESERIALIZATION_PRECISION, net_http2::HTTPStatusCode2::ERROR},
        {StatusCode::OV_INTERNAL_DESERIALIZATION_ERROR, net_http2::HTTPStatusCode2::ERROR},

        // Inference
        {StatusCode::OV_INTERNAL_INFERENCE_ERROR, net_http2::HTTPStatusCode2::ERROR},

        // Serialization

        // Should never occur - it should be validated during model loading
        {StatusCode::OV_UNSUPPORTED_SERIALIZATION_PRECISION, net_http2::HTTPStatusCode2::ERROR},
        {StatusCode::OV_INTERNAL_SERIALIZATION_ERROR, net_http2::HTTPStatusCode2::ERROR},

        // GetModelStatus
        {StatusCode::INTERNAL_ERROR, net_http2::HTTPStatusCode2::ERROR},

        // Binary input
        {StatusCode::INVALID_NO_OF_CHANNELS, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::BINARY_IMAGES_RESOLUTION_MISMATCH, net_http2::HTTPStatusCode2::BAD_REQUEST},
        {StatusCode::STRING_VAL_EMPTY, net_http2::HTTPStatusCode2::BAD_REQUEST},
    };
    auto it = httpStatusMap.find(status.getCode());
    if (it != httpStatusMap.end()) {
        return it->second;
    } else {
        return net_http2::HTTPStatusCode2::ERROR;
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
        //         // req->ReplyWithStatus(net_http2::HTTPStatusCode2::ERROR);
        //     }
        // };
         
            try {
                return   this->processRequest(req);
            } catch (...) {
                SPDLOG_DEBUG("Exception caught in REST request handler");
                // req->ReplyWithStatus(net_http2::HTTPStatusCode2::ERROR);
            }
        };
    }

private:
    void parseHeaders(const net_http2::HttpRequest* req, std::vector<std::pair<std::string, std::string>>* headers) {
        
        if (req->get_header_value("Inference-Header-Content-Length").size() > 0) {
            std::pair<std::string, std::string> header{"Inference-Header-Content-Length", req->get_header_value("Inference-Header-Content-Length")};
            headers->emplace_back(header);
        }
    }
    void processRequest(net_http2::HttpRequest* req) {
        net_http2::HttpResponse res;
        SPDLOG_DEBUG("REST request {}", req->uri_path());
        std::string body;
        int64_t num_bytes = 0;
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
        if (responseComponents.inferenceHeaderContentLength.has_value()) {
            std::pair<std::string, std::string> header{"Inference-Header-Content-Length", std::to_string(responseComponents.inferenceHeaderContentLength.value())};
            headers.emplace_back(header);
        }
        for (const auto& kv : headers) {
            req->OverwriteResponseHeader(kv.first, kv.second);
        }
        res.headers=res.ConvertVectorToMap(headers);
        res.body=output;
        return res;
        // req->WriteResponseString(output);
        // if (http_status != net_http2::HTTPStatusCode2::OK && http_status != net_http2::HTTPStatusCode2::CREATED) {
        //     SPDLOG_DEBUG("Processing HTTP/REST request failed: {} {}. Reason: {}",
        //         req->method,
        //         req->path,
        //         status.string());
        // }
        // req->ReplyWithStatus(http_status);
    }

    std::unique_ptr<HttpRestApiHandler> handler_;
};

std::unique_ptr<http_server> createAndStartHttpServer(const std::string& address, int port, int num_threads, ovms::Server& ovmsServer, int timeout_in_ms) {
    // auto options = std::make_unique<net_http::ServerOptions>();
    // options->AddPort(static_cast<uint32_t>(port));
    // options->SetAddress(address);
    // options->SetExecutor(std::make_unique<RequestExecutor>(num_threads));
    auto server =HttpServer::CreateHTTPServer(address, static_cast<uint32_t>(port), num_threads);
    // auto server = net_http::CreateEvHTTPServer(std::move(options));
    if (server == nullptr) {
        SPDLOG_ERROR("Failed to create http server");
        return nullptr;
    }

    std::shared_ptr<RestApiRequestDispatcher> dispatcher =
        std::make_shared<RestApiRequestDispatcher>(ovmsServer, timeout_in_ms);

    // net_http::RequestHandlerOptions handler_options;
    // server->RegisterRequestDispatcher(
    //     [dispatcher](net_http::ServerRequestInterface* req) {
    //         return dispatcher->dispatch(req);
    //     },
    //     handler_options);
    server->RegisterRequestDispatcher(dispatcher);
    // if (server->StartAcceptingRequests()) {
    //     SPDLOG_INFO("REST server listening on port {} with {} threads", port, num_threads);
    //     return server;
    // }
        if (server->StartAcceptingRequests()) {
        SPDLOG_INFO("REST server listening on port {} with {} threads", port, num_threads);
        return server;
    }

    return nullptr;
}
}  // namespace ovms
