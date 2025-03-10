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

class RestApiRequestDispatcher; // 前置声明

// HTTP 请求结构体
struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

// HTTP 响应结构体
struct HttpResponse {
    std::string version = "HTTP/1.1";
    int status_code = 200;
    std::string status_text = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;
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
            
            std::string response_str = build_response(res);
            send(client_fd, response_str.c_str(), response_str.size(), 0);
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
};

// 示例请求处理器
class RestApiRequestDispatcher {
public:
    RestApiRequestDispatcher(int timeout_ms) : m_timeout(timeout_ms) {}

    HttpResponse dispatch(const HttpRequest& req) {
        HttpResponse res;
        
        // 简单路由示例
        if (req.path == "/hello") {
            res.body = "Hello World from Linux!";
        } else if (req.path == "/status") {
            res.body = "Server Status: OK";
        } else if (req.path == "/headers") {
            std::ostringstream oss;
            for (const auto& [k, v] : req.headers) {
                oss << k << ": " << v << "\n";
            }
            res.body = oss.str();
        } else {
            res.status_code = 404;
            res.status_text = "Not Found";
            res.body = "404 Page Not Found";
        }

        res.headers["Content-Type"] = "text/plain";
        return res;
    }

private:
    int m_timeout;
};

int main() {
    HttpServer* server = HttpServer::CreateHTTPServer("0.0.0.0", 8080);
    RestApiRequestDispatcher* dispatcher = new RestApiRequestDispatcher(5000);
    
    server->RegisterRequestDispatcher(dispatcher);
    server->StartAcceptingRequests();

    return 0;
}