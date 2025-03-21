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
#pragma once
#include "status.hpp"
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <memory>
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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#include "tensorflow_serving/util/net_http/server/public/httpserver_interface.h"
#pragma GCC diagnostic pop

namespace ovms {
namespace net_http2 = ovms;
class Server;
class RestApiRequestDispatcher;
// HTTP 响应结构体
struct HttpResponse {
    std::string version = "HTTP/1.1";
    int status_code = 200;
    std::string status_text = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    std::unordered_map<std::string, std::string> ConvertVectorToMap(
        const std::vector<std::pair<std::string, std::string>>& headers_vec);
};
// HTTP 请求结构体
struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::string get_header_value(const std::string& key) const;
    void OverwriteResponseHeader(HttpResponse* res, const std::string& key, const std::string& value);
};
class HttpServer {
public:
    static HttpServer* CreateHTTPServer(const std::string& address, uint16_t port, int num_threads = 4);
    void RegisterRequestDispatcher(RestApiRequestDispatcher* dispatcher);
    void StartAcceptingRequests();
    void StopServer();
    ~HttpServer();

private:
    HttpServer(const std::string& address, uint16_t port, int num_threads);
    void start_server();
    void accept_connections();
    void handle_connection(int client_fd);
    HttpRequest parse_request(const std::string& raw_request);
    std::string url_decode(const std::string& str);
    std::string build_response(const HttpResponse& res);

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

enum class HTTPStatusCode2 {
    // These are the status codes we can give back to the client.
    // http://www.iana.org/assignments/http-status-codes/http-status-codes.xhtml#http-status-codes-1
    UNDEFINED = 0,  // Illegal value for initialization
    FIRST_CODE = 100,

    // Informational
    CONTINUE = 100,    // Continue
    SWITCHING = 101,   // Switching Protocols
    PROCESSING = 102,  // Processing (RFC 2518, sec 10.1)

    // Success
    OK = 200,                // OK
    CREATED = 201,           // Created
    ACCEPTED = 202,          // Accepted
    PROVISIONAL = 203,       // Non-Authoritative Information
    NO_CONTENT = 204,        // No Content
    RESET_CONTENT = 205,     // Reset Content
    PART_CONTENT = 206,      // Partial Content
    MULTI_STATUS = 207,      // Multi-Status (RFC 2518, sec 10.2)
    ALREADY_REPORTED = 208,  // Already Reported (RFC 5842)
    IM_USED = 226,           // IM Used (RFC 3229)

    // Redirect
    MULTIPLE = 300,           // Multiple Choices
    MOVED_PERM = 301,         // Moved Permanently
    MOVED_TEMP = 302,         // Found. For historical reasons,
                              // a user agent MAY change the method
                              // from POST to GET for the subsequent
                              // request (RFC 7231, sec 6.4.3).
    SEE_OTHER = 303,          // See Other
    NOT_MODIFIED = 304,       // Not Modified
    USE_PROXY = 305,          // Use Proxy
    TEMP_REDIRECT = 307,      // Similar to 302, except that user
                              // agents MUST NOT change the request
                              // method (RFC 7231, sec 6.4.7)
    RESUME_INCOMPLETE = 308,  // Resume Incomplete

    // Client Error
    BAD_REQUEST = 400,            // Bad Request
    UNAUTHORIZED = 401,           // Unauthorized
    PAYMENT = 402,                // Payment Required
    FORBIDDEN = 403,              // Forbidden
    NOT_FOUND = 404,              // Not Found
    METHOD_NA = 405,              // Method Not Allowed
    NONE_ACC = 406,               // Not Acceptable
    PROXY = 407,                  // Proxy Authentication Required
    REQUEST_TO = 408,             // Request Time-out
    CONFLICT = 409,               // Conflict
    GONE = 410,                   // Gone
    LEN_REQUIRED = 411,           // Length Required
    PRECOND_FAILED = 412,         // Precondition Failed
    ENTITY_TOO_BIG = 413,         // Request Entity Too Large
    URI_TOO_BIG = 414,            // Request-URI Too Large
    UNKNOWN_MEDIA = 415,          // Unsupported Media Type
    BAD_RANGE = 416,              // Requested range not satisfiable
    BAD_EXPECTATION = 417,        // Expectation Failed
    IM_A_TEAPOT = 418,            // I'm a Teapot (RFC 2324, 7168)
    MISDIRECTED_REQUEST = 421,    // Misdirected Request (RFC 7540)
    UNPROC_ENTITY = 422,          // Unprocessable Entity (RFC 2518, sec 10.3)
    LOCKED = 423,                 // Locked (RFC 2518, sec 10.4)
    FAILED_DEP = 424,             // Failed Dependency (RFC 2518, sec 10.5)
    UPGRADE_REQUIRED = 426,       // Upgrade Required (RFC 7231, sec 6.5.14)
    PRECOND_REQUIRED = 428,       // Precondition Required (RFC 6585, sec 3)
    TOO_MANY_REQUESTS = 429,      // Too Many Requests (RFC 6585, sec 4)
    HEADER_TOO_LARGE = 431,       // Request Header Fields Too Large
                                  // (RFC 6585, sec 5)
    UNAVAILABLE_LEGAL = 451,      // Unavailable For Legal Reasons (RFC 7725)
    CLIENT_CLOSED_REQUEST = 499,  // Client Closed Request (Nginx)

    // Server Error
    ERROR = 500,               // Internal Server Error
    NOT_IMP = 501,             // Not Implemented
    BAD_GATEWAY = 502,         // Bad Gateway
    SERVICE_UNAV = 503,        // Service Unavailable
    GATEWAY_TO = 504,          // Gateway Time-out
    BAD_VERSION = 505,         // HTTP Version not supported
    VARIANT_NEGOTIATES = 506,  // Variant Also Negotiates (RFC 2295)
    INSUF_STORAGE = 507,       // Insufficient Storage (RFC 2518, sec 10.6)
    LOOP_DETECTED = 508,       // Loop Detected (RFC 5842)
    NOT_EXTENDED = 510,        // Not Extended (RFC 2774)
    NETAUTH_REQUIRED = 511,    // Network Authentication Required
                               // (RFC 6585, sec 6)
    LAST_CODE = 599,
};
const HTTPStatusCode2 http(const ovms::Status& status);

// class Status {
// private:
//     StatusCode code;  // 存储状态码的成员变量

// public:
//     // 构造函数，用于初始化状态码
//     Status(StatusCode c);

//     // 获取状态码的方法
//     StatusCode getCode() const;
// };

// const HTTPStatusCode http(const ovms::Status& status);

/**
 * @brief Creates a and starts Http Server
 *
 * @param port
 * @param num_threads
 * @param timeout_in_m not implemented
 *
 * @return std::unique_ptr<http_server>
 */
std::unique_ptr<HttpServer> createAndStartHttpServer(const std::string& address, int port, int num_threads, ovms::Server& ovmsServer, int timeout_in_ms = -1);

}  // namespace ovms
