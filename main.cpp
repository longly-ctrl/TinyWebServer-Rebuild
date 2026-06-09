#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <map>
#include <mysql/mysql.h>

const int PORT = 9006;
const int MAX_EVENT_NUMBER = 1024;
const int BUFFER_SIZE = 4096;
const std::string ROOT_PATH = "./root";
const char* DB_HOST = "localhost";
const char* DB_USER = "tinyweb";
const char* DB_PASSWORD = "123123";
const char* DB_DATABASE = "yourdb";
const unsigned int DB_PORT = 3306;

MYSQL* mysql_conn = nullptr;

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
	int new_option = old_option | O_NONBLOCK;
	fcntl(fd, F_SETFL, new_option);

	return old_option;
}

void add_fd(int epoll_fd, int fd) {
	epoll_event event;
	event.data.fd = fd;
	event.events = EPOLLIN;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);
	set_non_blocking(fd);
}

std::string get_file_type(const std::string& file_path) {
	if(file_path.size() >= 5 && file_path.substr(file_path.size() - 5) == ".html") {
		return "text/html; charset=UTF-8";
	}

	if(file_path.size() >= 4 && file_path.substr(file_path.size() - 4) == ".css") {
		return "text/css";
	}

	if(file_path.size() >= 3 && file_path.substr(file_path.size() - 3) == ".js") {
		return "application/javascript";
	}

	if(file_path.size() >= 4 && file_path.substr(file_path.size() - 4) == ".jpg") {
		return "image/jpeg";
	}

	if(file_path.size() >= 5 && file_path.substr(file_path.size() - 5) == ".jpeg") {
		return "image/jpeg";
	}

	if(file_path.size() >= 4 && file_path.substr(file_path.size() - 4) == ".png") {
		return "image/png";
	}

	return "text/plain; charset=UTF-8";
}

bool read_file(const std::string& file_path, std::string& body) {
	std::ifstream file(file_path, std::ios::in | std::ios::binary);

	if(!file.is_open()) {
		return false;
	}

	std::ostringstream content;

	content << file.rdbuf();

	body = content.str();

	return true;
}

std::string make_error_response(const std::string& status, const std::string& message) {
	std::string body = 
		"<!DOCTYPE html>"
		"<html>"
		"<head><meta charset=\"UTF-8\"><title>" + status + "</title></head>"
		"<body><h1>" + message + "</h1></body>"
		"</html>";

	return "HTTP/1.1" + status + "\r\n"
		"Content-Type: text/html; charset=UTF-8\r\n"
		"Content-Length: " + std::to_string(body.size()) + "\r\n"
		"Connection: close\r\n"
		"\r\n" +
		body;
}

bool parse_request_line(const std::string& line, HttpRequest& request) {
	std::istringstream line_stream(line);

	line_stream >> request.method >> request.path >> request.version;

	if(request.method.empty() || request.path.empty() || request.version.empty()) {
		return false;
	}

	if(request.method != "GET" && request.method != "POST") {
		return false;
	}

	if(request.version != "HTTP/1.1" && request.version != "HTTP/1.0") {
		return false;
	}

	if(request.path == "/") {
		request.path = "/index.html";
	}

	if(request.path.find("..") != std::string::npos) {
		return false;
	}

	return true;
}

bool parse_header_line(const std::string line, HttpRequest& request) {
	size_t colon_pos = line.find(':');

	if(colon_pos == std::string::npos) {
		return false;
	}
	
	std::string key = line.substr(0, colon_pos);

	std::string value = line.substr(colon_pos + 1);

	while (!value.empty() && value.front() == ' ') {
		value.erase(value.begin());
	}

	request.headers[key] = value;

	if(key == "Content-Length") {
		request.content_length = std::stoi(value);
	}

	return true;

}

std::map<std::string, std::string> parse_form_body(const std::string& body) {
	std::map<std::string, std::string> form;

	std::istringstream body_stream(body);

	std::string pair;

	while (std::getline(body_stream, pair, '&')) {
		size_t equal_pos = pair.find('=');

		if(equal_pos == std::string::npos) {
			continue;
		}

		std::string key = pair.substr(0, equal_pos);
		
		std::string value = pair.substr(equal_pos + 1);

		form[key] = value;
	}

	return form;	

}

HttpRequest parse_http_request(const std::string& raw_request) {
	HttpRequest request;

	HttpParseState state = HttpParseState::REQUEST_LINE;

	size_t line_start = 0;

	while (true) {
		size_t line_end = raw_request.find("\r\n", line_start);

		if(line_end == std::string::npos) {
			break;
		}

		std::string line = raw_request.substr(line_start, line_end - line_start);

		line_start = line_end + 2;

		if(state == HttpParseState::REQUEST_LINE) {
			if(!parse_request_line(line, request)) {
				return request;
			}

			state = HttpParseState::REQUEST_HEADER;
		}else if(state == HttpParseState::REQUEST_HEADER) {
			if(line.empty()) {
				if(request.method == "POST" && request.content_length > 0) {
					state = HttpParseState::REQUEST_BODY;
				}else {
					state = HttpParseState::REQUEST_DONE;
				}

				break;
			}

			if(!parse_header_line(line, request)) {
				return request;
			}
		}
	}

	if(state == HttpParseState::REQUEST_BODY) {
		if(line_start + request.content_length > raw_request.size()) {
			return request;
		}

		request.body = raw_request.substr(line_start, request.content_length);

		state = HttpParseState::REQUEST_DONE;
	}
	if(state == HttpParseState::REQUEST_DONE) {
		request.valid = true;
	}

	return request;
}

bool init_mysql(){
	mysql_conn = mysql_init(nullptr);

	if(mysql_conn == nullptr) {
		std::cerr << "mysql_init failed\n";
		return false;
	}

	MYSQL* result = mysql_real_connect(
			mysql_conn,
			DB_HOST,
			DB_USER,
			DB_PASSWORD,
			DB_DATABASE,
			DB_PORT,
			nullptr,
			0
			);

	if(result == nullptr){
		std::cerr << "mysql_real_connect failed: " << mysql_error(mysql_conn) << '\n';
		mysql_close(mysql_conn);
		mysql_conn = nullptr;
		return false;
	}

	mysql_set_character_set(mysql_conn, "utf8mb4");

	std::cout << "mysql connected\n";

	return true;
}

std::string mysql_escape(const std::string& text) {
	std::string escaped;

	escaped.resize(text.size() * 2 + 1);
	
	unsigned long length = mysql_real_escape_string(
			mysql_conn,
			&escaped[0],
			text.c_str(),
			text.size()
			);

	escaped.resize(length);

	return escaped;
}

bool mysql_user_exists(const std::string& username) {
	std::string safe_username = mysql_escape(username);

	std::string sql =
		"SELECT username FROM user WHERE username='" + safe_username + "' LIMIT 1";

	int ret = mysql_query(mysql_conn, sql.c_str());

	if(ret != 0){
		std::cerr << "mysql_query failed: " << mysql_error(mysql_conn) << '\n';
		return false;
	}

	MYSQL_RES* result = mysql_store_result(mysql_conn);

	if(result == nullptr) {
		return false;
	}

	MYSQL_ROW row = mysql_fetch_row(result);

	bool exists = (row != nullptr);

	mysql_free_result(result);

	return exists;
}

bool mysql_check_login(const std::string& username, const std::string& password){
	std::string safe_username = mysql_escape(username);

	std::string safe_password = mysql_escape(password);

	std::string sql =
		"SELECT username FROM user WHERE username='" + safe_username + "' AND passwd='" +
		safe_password + "' LIMIT 1";

	int ret = mysql_query(mysql_conn, sql.c_str());

	if(ret != 0){
		std::cout << "mysql_query failed: " << mysql_error(mysql_conn) << '\n';
		return false;
	}

	MYSQL_RES* result = mysql_store_result(mysql_conn);

	if(result == nullptr){
		return false;
	}

	MYSQL_ROW row = mysql_fetch_row(result);

	bool success = (row != nullptr);

	mysql_free_result(result);

	return success;
}

bool mysql_register_user(const std::string& username, std::string& password) {
	if(mysql_user_exists(username)) {
		return false;
	}

	std::string safe_username = mysql_escape(username);
	std::string safe_password = mysql_escape(password);

	std::string sql = 
		"INSERT INTO suer(username, password) VALUES('" +
		safe_username + "', '" + safe_password + "')";

	int ret = mysql_query(mysql_conn, sql.c_str());

	if(ret != 0) {
		std::cerr << "mysql_query failed: " << mysql_error(mysql_conn) << '\n';
		return false;
	}

	return true;
}


std::string build_response(const std::string& raw_request) {
       HttpRequest request = parse_http_request(raw_request);

       if(!request.valid) {
	       return make_error_response("400 Bad Request", "400 Bad Request");
       }

       if(request.method == "POST") {
	       std::map<std::string, std::string> form = parse_form_body(request.body);

	       std::string username = form["username"];

	       std::string password = form["password"];

	       if(username.empty() || password.empty()) {
		       return make_error_response("400 Bad Request", "username or password empty.");
	       }

	       std::string message;

	       if(request.path == "./login") {
		       bool success = mysql_check_login(username, password);

		       if(success) {
			       message = "login success";
		       }else{
			       message = "login failed";
		       }
	       }else if(request.path == "./register") {
		       bool success = mysql_register_user(username, password);

		       if(success) {
			       message = "register success";
		       }else{
			       message = "register failed";
		       }
	       }
	       else{
		       return make_error_response("404 Not Found", "404 Not Found");
	       }
		     



	       std::string body = 
		       "<!DOCTYPE html>"
		       "<html>"
		       "<head><meta charset=\"UTF-8\"><title>POST Result</title></head>"
		       "<body>"
		       "<h1>POST request received</h1>"
		       "<p>path: " + request.path + "</p>"
		       "<p>username: " + username + "</p>"
		       "<p>password: " + password + "</p>"

		       "</html>";

	       return "HTTP/1.1 200 OK\r\n"
		      "Content-Type: text/html; charset=UTF-8\r\n"
		      "Content-Length: " + std::to_string(body.size()) + "\r\n"
		      "Connection: close\r\n"
		      "\r\n" + 
		      body;
       }
	
       std::string file_path = ROOT_PATH + request.path;

       std::string body;

       bool success = read_file(file_path, body);

       if(!success) {
	       return make_error_response("404 Not Found", "404 Not Found");
       }

       std::string content_type = get_file_type(file_path);

       return "HTTP/1.1 200 OK\r\n"
	      "Content-Type: " + content_type + "\r\n"
	      "Content-Length: " + std::to_string(body.size()) + "\r\n"
	      "Connection: close\r\n"
	      "\r\n" + 
	      body;
}



int main() {

	if(!init_mysql()) {
		return 1;
	}
	int listen_fd = socket(AF_INET, SOCK_STREAM, 0);

	if(listen_fd == -1) {
		std::cerr << "socket failed\n";
		return 1;
	}

	int reuse = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	sockaddr_in address;
	std::memset(&address, 0, sizeof(address));

	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(PORT);

	int ret = bind(listen_fd, (sockaddr*)&address, sizeof(address));

	if(ret == -1) {
		std::cerr << "bind failed\n";
		close(listen_fd);
		return 1;
	}

	ret = listen(listen_fd, 5);

	if(ret == -1) {
		std::cerr << "listen failed\n";
		close(listen_fd);
		return 1;
	}
	
	epoll_event events[MAX_EVENT_NUMBER];

	int epoll_fd = epoll_create(5);

	if(epoll_fd == -1) {
		std::cerr << "epoll_create failed\n";
		close(listen_fd);
		return 1;
	}

	add_fd(epoll_fd, listen_fd);

	std::cout << "server start at port " << PORT << "\n";

	while(true) {
		int number = epoll_wait(epoll_fd, events, MAX_EVENT_NUMBER, -1);

		if(number < 0 && errno != EINTR) {
			std::cerr << "epoll_wait failed\n";
			break;
		}

		for(int i = 0; i < number; ++i) {
			int sock_fd = events[i].data.fd;

			if(sock_fd == listen_fd) {

				sockaddr_in client_address;
				socklen_t client_len = sizeof(client_address);

				int client_fd = accept(listen_fd, (sockaddr*)&client_address, &client_len);
				if(client_fd == -1) {
					std::cerr << "accept failed\n";
					continue;
				}

				add_fd(epoll_fd, client_fd);
			}else if(events[i].events & EPOLLIN) {
				char buffer[BUFFER_SIZE];
				std::memset(buffer, 0, sizeof(buffer));
				//return byte numbers
				int bytes_read = read(sock_fd, buffer, sizeof(buffer) - 1);

				if(bytes_read <= 0) {
					close(sock_fd);
					continue;
				}

				std::string raw_request(buffer);

				std::cout << "request:\n" << raw_request << std::endl;

				std::string response = build_response(raw_request);

				write(sock_fd, response.c_str(), response.size());

				close(sock_fd);
			}
		}
	}
	close(epoll_fd);
	close(listen_fd);

	if(mysql_conn != nullptr) {
		mysql_close(mysql_conn);
	}
	return 0;
}

