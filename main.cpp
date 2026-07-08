#include <arpa/inet.h>
#include <cerrno>
#include <condition_variable>
#include <ctime>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <mysql/mysql.h>
#include <queue>
#include <sstream>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

struct ServerConfig {
    int port = 9006;
    int thread_num = 8;
    int sql_num = 8;
    int timeout_seconds = 30;
    std::string root_path = "./root";
};

const int MAX_EVENT_NUMBER = 1024;
const int BUFFER_SIZE = 8192;
const char* DB_HOST = "localhost";
const char* DB_USER = "tinyweb";
const char* DB_PASSWORD = "123123";
const char* DB_DATABASE = "yourdb";
const unsigned int DB_PORT = 3306;

ServerConfig g_config;

class Logger {
public:
    void info(const std::string& message) {
        write("INFO", message);
    }

    void error(const std::string& message) {
        write("ERROR", message);
    }

private:
    void write(const std::string& level, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::cout << "[" << level << "] " << message << std::endl;
    }

    std::mutex mutex_;
};

Logger g_logger;

enum class HttpParseState {
    REQUEST_LINE,
    REQUEST_HEADER,
    REQUEST_BODY,
    REQUEST_DONE
};

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string body;
    int content_length = 0;
    bool valid = false;
};

int set_non_blocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    if (old_option == -1) {
        return -1;
    }

    int new_option = old_option | O_NONBLOCK;
    if (fcntl(fd, F_SETFL, new_option) == -1) {
        return -1;
    }

    return old_option;
}

bool add_fd(int epoll_fd, int fd) {
    epoll_event event;
    std::memset(&event, 0, sizeof(event));
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == -1) {
        return false;
    }

    return set_non_blocking(fd) != -1;
}

void remove_fd(int epoll_fd, int fd) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
}

void close_fd(int epoll_fd, int fd) {
    remove_fd(epoll_fd, fd);
    close(fd);
}

std::string get_file_type(const std::string& file_path) {
    if (file_path.size() >= 5 && file_path.substr(file_path.size() - 5) == ".html") {
        return "text/html; charset=UTF-8";
    }
    if (file_path.size() >= 4 && file_path.substr(file_path.size() - 4) == ".css") {
        return "text/css; charset=UTF-8";
    }
    if (file_path.size() >= 3 && file_path.substr(file_path.size() - 3) == ".js") {
        return "application/javascript; charset=UTF-8";
    }
    if (file_path.size() >= 4 && file_path.substr(file_path.size() - 4) == ".jpg") {
        return "image/jpeg";
    }
    if (file_path.size() >= 5 && file_path.substr(file_path.size() - 5) == ".jpeg") {
        return "image/jpeg";
    }
    if (file_path.size() >= 4 && file_path.substr(file_path.size() - 4) == ".png") {
        return "image/png";
    }
    if (file_path.size() >= 4 && file_path.substr(file_path.size() - 4) == ".ico") {
        return "image/x-icon";
    }
    return "text/plain; charset=UTF-8";
}

bool read_file(const std::string& file_path, std::string& body) {
    std::ifstream file(file_path.c_str(), std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    std::ostringstream content;
    content << file.rdbuf();
    body = content.str();
    return true;
}

std::string make_response(const std::string& status, const std::string& content_type, const std::string& body) {
    return "HTTP/1.1 " + status + "\r\n"
           "Content-Type: " + content_type + "\r\n"
           "Content-Length: " + std::to_string(body.size()) + "\r\n"
           "Connection: close\r\n"
           "\r\n" +
           body;
}

std::string make_error_response(const std::string& status, const std::string& message) {
    std::string body =
        "<!DOCTYPE html>"
        "<html>"
        "<head><meta charset=\"UTF-8\"><title>" + status + "</title></head>"
        "<body><h1>" + message + "</h1><p><a href=\"/index.html\">Back to home</a></p></body>"
        "</html>";

    return make_response(status, "text/html; charset=UTF-8", body);
}

bool parse_request_line(const std::string& line, HttpRequest& request) {
    std::istringstream line_stream(line);
    line_stream >> request.method >> request.path >> request.version;

    if (request.method.empty() || request.path.empty() || request.version.empty()) {
        return false;
    }
    if (request.method != "GET" && request.method != "POST") {
        return false;
    }
    if (request.version != "HTTP/1.1" && request.version != "HTTP/1.0") {
        return false;
    }

    size_t query_pos = request.path.find('?');
    if (query_pos != std::string::npos) {
        request.path = request.path.substr(0, query_pos);
    }
    if (request.path == "/") {
        request.path = "/index.html";
    }
    if (request.path.find("..") != std::string::npos) {
        return false;
    }

    return true;
}

bool parse_header_line(const std::string& line, HttpRequest& request) {
    size_t colon_pos = line.find(':');
    if (colon_pos == std::string::npos) {
        return false;
    }

    std::string key = line.substr(0, colon_pos);
    std::string value = line.substr(colon_pos + 1);

    while (!value.empty() && value.front() == ' ') {
        value.erase(value.begin());
    }

    request.headers[key] = value;

    if (key == "Content-Length") {
        try {
            request.content_length = std::stoi(value);
        } catch (...) {
            return false;
        }
    }

    return true;
}

HttpRequest parse_http_request(const std::string& raw_request) {
    HttpRequest request;
    HttpParseState state = HttpParseState::REQUEST_LINE;
    size_t line_start = 0;

    while (true) {
        size_t line_end = raw_request.find("\r\n", line_start);
        if (line_end == std::string::npos) {
            break;
        }

        std::string line = raw_request.substr(line_start, line_end - line_start);
        line_start = line_end + 2;

        if (state == HttpParseState::REQUEST_LINE) {
            if (!parse_request_line(line, request)) {
                return request;
            }
            state = HttpParseState::REQUEST_HEADER;
        } else if (state == HttpParseState::REQUEST_HEADER) {
            if (line.empty()) {
                if (request.method == "POST" && request.content_length > 0) {
                    state = HttpParseState::REQUEST_BODY;
                } else {
                    state = HttpParseState::REQUEST_DONE;
                }
                break;
            }
            if (!parse_header_line(line, request)) {
                return request;
            }
        }
    }

    if (state == HttpParseState::REQUEST_BODY) {
        if (request.content_length < 0 || line_start + static_cast<size_t>(request.content_length) > raw_request.size()) {
            return request;
        }
        request.body = raw_request.substr(line_start, request.content_length);
        state = HttpParseState::REQUEST_DONE;
    }

    request.valid = (state == HttpParseState::REQUEST_DONE);
    return request;
}

int hex_to_int(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::string url_decode(const std::string& text) {
    std::string result;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            result.push_back(' ');
        } else if (text[i] == '%' && i + 2 < text.size()) {
            int high = hex_to_int(text[i + 1]);
            int low = hex_to_int(text[i + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>(high * 16 + low));
                i += 2;
            } else {
                result.push_back(text[i]);
            }
        } else {
            result.push_back(text[i]);
        }
    }
    return result;
}

std::map<std::string, std::string> parse_form_body(const std::string& body) {
    std::map<std::string, std::string> form;
    std::istringstream body_stream(body);
    std::string pair;

    while (std::getline(body_stream, pair, '&')) {
        size_t equal_pos = pair.find('=');
        if (equal_pos == std::string::npos) {
            continue;
        }

        std::string key = url_decode(pair.substr(0, equal_pos));
        std::string value = url_decode(pair.substr(equal_pos + 1));
        form[key] = value;
    }

    return form;
}

class MysqlConnectionPool {
public:
    bool init(int connection_count) {
        for (int i = 0; i < connection_count; ++i) {
            MYSQL* conn = mysql_init(nullptr);
            if (conn == nullptr) {
                g_logger.error("mysql_init failed");
                return false;
            }

            MYSQL* result = mysql_real_connect(
                conn,
                DB_HOST,
                DB_USER,
                DB_PASSWORD,
                DB_DATABASE,
                DB_PORT,
                nullptr,
                0
            );

            if (result == nullptr) {
                g_logger.error(std::string("mysql_real_connect failed: ") + mysql_error(conn));
                mysql_close(conn);
                close_pool();
                return false;
            }

            mysql_set_character_set(conn, "utf8mb4");
            connections_.push(conn);
        }

        g_logger.info("mysql pool initialized: " + std::to_string(connection_count));
        return true;
    }

    MYSQL* get_connection() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this]() {
            return !connections_.empty();
        });

        MYSQL* conn = connections_.front();
        connections_.pop();
        return conn;
    }

    void release_connection(MYSQL* conn) {
        if (conn == nullptr) {
            return;
        }

        {
            std::unique_lock<std::mutex> lock(mutex_);
            connections_.push(conn);
        }

        condition_.notify_one();
    }

    void close_pool() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!connections_.empty()) {
            MYSQL* conn = connections_.front();
            connections_.pop();
            mysql_close(conn);
        }
    }

private:
    std::queue<MYSQL*> connections_;
    std::mutex mutex_;
    std::condition_variable condition_;
};

MysqlConnectionPool g_mysql_pool;

class MysqlConnectionRAII {
public:
    MysqlConnectionRAII(MYSQL** conn, MysqlConnectionPool* pool) : conn_(nullptr), pool_(pool) {
        *conn = pool_->get_connection();
        conn_ = *conn;
    }

    ~MysqlConnectionRAII() {
        if (conn_ != nullptr) {
            pool_->release_connection(conn_);
        }
    }

private:
    MYSQL* conn_;
    MysqlConnectionPool* pool_;
};

std::string mysql_escape(MYSQL* conn, const std::string& text) {
    std::string escaped;
    escaped.resize(text.size() * 2 + 1);

    unsigned long length = mysql_real_escape_string(
        conn,
        &escaped[0],
        text.c_str(),
        text.size()
    );

    escaped.resize(length);
    return escaped;
}

bool mysql_user_exists(MYSQL* conn, const std::string& username) {
    std::string safe_username = mysql_escape(conn, username);
    std::string sql = "SELECT username FROM user WHERE username='" + safe_username + "' LIMIT 1";

    if (mysql_query(conn, sql.c_str()) != 0) {
        g_logger.error(std::string("mysql_query failed: ") + mysql_error(conn));
        return false;
    }

    MYSQL_RES* result = mysql_store_result(conn);
    if (result == nullptr) {
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    bool exists = (row != nullptr);
    mysql_free_result(result);
    return exists;
}

bool mysql_check_login(MYSQL* conn, const std::string& username, const std::string& password) {
    std::string safe_username = mysql_escape(conn, username);
    std::string safe_password = mysql_escape(conn, password);
    std::string sql =
        "SELECT username FROM user WHERE username='" + safe_username +
        "' AND passwd='" + safe_password + "' LIMIT 1";

    if (mysql_query(conn, sql.c_str()) != 0) {
        g_logger.error(std::string("mysql_query failed: ") + mysql_error(conn));
        return false;
    }

    MYSQL_RES* result = mysql_store_result(conn);
    if (result == nullptr) {
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    bool success = (row != nullptr);
    mysql_free_result(result);
    return success;
}

bool mysql_register_user(MYSQL* conn, const std::string& username, const std::string& password) {
    if (mysql_user_exists(conn, username)) {
        return false;
    }

    std::string safe_username = mysql_escape(conn, username);
    std::string safe_password = mysql_escape(conn, password);
    std::string sql =
        "INSERT INTO user(username, passwd) VALUES('" +
        safe_username + "', '" + safe_password + "')";

    if (mysql_query(conn, sql.c_str()) != 0) {
        g_logger.error(std::string("mysql_query failed: ") + mysql_error(conn));
        return false;
    }

    return true;
}

std::string build_post_response(const HttpRequest& request) {
    std::map<std::string, std::string> form = parse_form_body(request.body);
    std::string username = form["username"];
    std::string password = form["password"];

    if (username.empty() || password.empty()) {
        return make_error_response("400 Bad Request", "username or password empty");
    }

    MYSQL* conn = nullptr;
    MysqlConnectionRAII mysql_guard(&conn, &g_mysql_pool);

    std::string message;
    if (request.path == "/login") {
        message = mysql_check_login(conn, username, password) ? "login success" : "login failed";
    } else if (request.path == "/register") {
        message = mysql_register_user(conn, username, password) ? "register success" : "register failed, user may already exist";
    } else {
        return make_error_response("404 Not Found", "404 Not Found");
    }

    std::string body =
        "<!DOCTYPE html>"
        "<html>"
        "<head><meta charset=\"UTF-8\"><title>Result</title><link rel=\"stylesheet\" href=\"/style.css\"></head>"
        "<body>"
        "<main class=\"auth-layout\">"
        "<section class=\"panel auth-card\">"
        "<p class=\"eyebrow\">Request result</p>"
        "<h1>" + message + "</h1>"
        "<p class=\"auth-copy\">username: " + username + "</p>"
        "<a class=\"button button-primary\" href=\"/index.html\">Back to home</a>"
        "</section>"
        "</main>"
        "</body>"
        "</html>";

    return make_response("200 OK", "text/html; charset=UTF-8", body);
}

std::string build_get_response(const HttpRequest& request) {
    std::string file_path = g_config.root_path + request.path;
    std::string body;

    if (!read_file(file_path, body)) {
        return make_error_response("404 Not Found", "404 Not Found");
    }

    return make_response("200 OK", get_file_type(file_path), body);
}

std::string build_response(const std::string& raw_request) {
    HttpRequest request = parse_http_request(raw_request);

    if (!request.valid) {
        return make_error_response("400 Bad Request", "400 Bad Request");
    }

    if (request.method == "POST") {
        return build_post_response(request);
    }

    return build_get_response(request);
}

class ThreadPool {
public:
    explicit ThreadPool(int thread_count) : stop_(false) {
        for (int i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this]() {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        condition_.wait(lock, [this]() {
                            return stop_ || !tasks_.empty();
                        });

                        if (stop_ && tasks_.empty()) {
                            return;
                        }

                        task = tasks_.front();
                        tasks_.pop();
                    }
                    task();
                }
            });
        }
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stop_ = true;
        }

        condition_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void append(std::function<void()> task) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            tasks_.push(task);
        }
        condition_.notify_one();
    }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stop_;
};

bool write_all(int fd, const std::string& response) {
    size_t written = 0;
    while (written < response.size()) {
        ssize_t ret = write(fd, response.data() + written, response.size() - written);
        if (ret > 0) {
            written += static_cast<size_t>(ret);
        } else if (ret == -1 && errno == EINTR) {
            continue;
        } else if (ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            std::this_thread::yield();
        } else {
            return false;
        }
    }
    return true;
}

void handle_client(int fd, const std::string& request) {
    std::string response = build_response(request);
    write_all(fd, response);
    close(fd);
}

void cleanup_idle_connections(int epoll_fd, std::unordered_map<int, std::time_t>& active_connections) {
    std::time_t now = std::time(nullptr);
    std::vector<int> expired;

    for (const auto& item : active_connections) {
        if (now - item.second >= g_config.timeout_seconds) {
            expired.push_back(item.first);
        }
    }

    for (int fd : expired) {
        g_logger.info("close idle fd: " + std::to_string(fd));
        active_connections.erase(fd);
        close_fd(epoll_fd, fd);
    }
}

void accept_clients(int listen_fd, int epoll_fd, std::unordered_map<int, std::time_t>& active_connections) {
    while (true) {
        sockaddr_in client_address;
        socklen_t client_len = sizeof(client_address);
        int client_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&client_address), &client_len);

        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            g_logger.error("accept failed: " + std::string(std::strerror(errno)));
            break;
        }

        if (!add_fd(epoll_fd, client_fd)) {
            g_logger.error("add client fd failed");
            close(client_fd);
            continue;
        }

        active_connections[client_fd] = std::time(nullptr);
    }
}

bool parse_args(int argc, char* argv[]) {
    int opt = 0;
    while ((opt = getopt(argc, argv, "p:t:s:r:o:")) != -1) {
        switch (opt) {
            case 'p':
                g_config.port = std::atoi(optarg);
                break;
            case 't':
                g_config.thread_num = std::atoi(optarg);
                break;
            case 's':
                g_config.sql_num = std::atoi(optarg);
                break;
            case 'r':
                g_config.root_path = optarg;
                break;
            case 'o':
                g_config.timeout_seconds = std::atoi(optarg);
                break;
            default:
                return false;
        }
    }

    if (g_config.port <= 0 || g_config.thread_num <= 0 || g_config.sql_num <= 0 || g_config.timeout_seconds <= 0) {
        return false;
    }
    return true;
}

int main(int argc, char* argv[]) {
    if (!parse_args(argc, argv)) {
        std::cerr << "usage: ./server [-p port] [-t thread_num] [-s sql_num] [-r root_path] [-o timeout_seconds]\n";
        return 1;
    }

    if (!g_mysql_pool.init(g_config.sql_num)) {
        return 1;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        g_logger.error("socket failed");
        g_mysql_pool.close_pool();
        return 1;
    }

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(g_config.port);

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
        g_logger.error("bind failed: " + std::string(std::strerror(errno)));
        close(listen_fd);
        g_mysql_pool.close_pool();
        return 1;
    }

    if (listen(listen_fd, 128) == -1) {
        g_logger.error("listen failed: " + std::string(std::strerror(errno)));
        close(listen_fd);
        g_mysql_pool.close_pool();
        return 1;
    }

    int epoll_fd = epoll_create(5);
    if (epoll_fd == -1) {
        g_logger.error("epoll_create failed");
        close(listen_fd);
        g_mysql_pool.close_pool();
        return 1;
    }

    if (!add_fd(epoll_fd, listen_fd)) {
        g_logger.error("add listen fd failed");
        close(epoll_fd);
        close(listen_fd);
        g_mysql_pool.close_pool();
        return 1;
    }

    epoll_event events[MAX_EVENT_NUMBER];
    std::unordered_map<int, std::time_t> active_connections;

    g_logger.info("server start at port " + std::to_string(g_config.port));

    {
        ThreadPool pool(g_config.thread_num);

        while (true) {
            int number = epoll_wait(epoll_fd, events, MAX_EVENT_NUMBER, 5000);

            if (number < 0 && errno != EINTR) {
                g_logger.error("epoll_wait failed");
                break;
            }

            cleanup_idle_connections(epoll_fd, active_connections);

            for (int i = 0; i < number; ++i) {
                int sock_fd = events[i].data.fd;

                if (sock_fd == listen_fd) {
                    accept_clients(listen_fd, epoll_fd, active_connections);
                    continue;
                }

                if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    active_connections.erase(sock_fd);
                    close_fd(epoll_fd, sock_fd);
                    continue;
                }

                if (events[i].events & EPOLLIN) {
                    char buffer[BUFFER_SIZE];
                    std::memset(buffer, 0, sizeof(buffer));
                    int bytes_read = read(sock_fd, buffer, sizeof(buffer));

                    if (bytes_read <= 0) {
                        active_connections.erase(sock_fd);
                        close_fd(epoll_fd, sock_fd);
                        continue;
                    }

                    std::string raw_request(buffer, bytes_read);
                    active_connections.erase(sock_fd);
                    remove_fd(epoll_fd, sock_fd);

                    pool.append([sock_fd, raw_request]() {
                        handle_client(sock_fd, raw_request);
                    });
                }
            }
        }
    }

    close(epoll_fd);
    close(listen_fd);
    g_mysql_pool.close_pool();
    return 0;
}
