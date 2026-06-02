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

const int PORT = 9006;
const int MAX_EVENT_NUMBER = 1024;
const int BUFFER_SIZE = 4096;
const std::string ROOT_PATH = "./root";

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
	if(file_path.size() >= 5 && file_path.substr(file_path.size.() - 5) == ".html") {
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

std::string read_file(const std::string& file_path) {
	std::ifstream file(file_path, std::ios::in | std::ios::binary);

	if(!file.is_open()) {
		return "";
	}

	std::ostringstream content;

	content << file.rdbuf();

	return content.str();
}

std::string parse_request_path(const std::string& request) {
	std::istringstream request_stream(request);

	std::string method;
	std::string path;
	std::string version;

	request_stream >> method >> path >> version;

	if(method != "GET") {
		reurn "";
	}

	if(path == "/") {
		path = "/index.html";
	}

	return path;
}


std::string build_response(const std::string& request) {

	std::string path = parse_request_path(request);

	if(path.empty()) {
		std::string body = "<h1>400 Bad Request</h1>";

	return 
}

int main() {
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

				int bytes_read = read(sock_fd, buffer, sizeof(buffer) - 1);

				if(bytes_read <= 0) {
					close(sock_fd);
					continue;
				}

				std::cout << "request:\n" << buffer << std::endl;

				std::string response = build_response();

				write(sock_fd, response.c_str(), response.size());

				close(sock_fd);
			}
		}
	}
	close(epoll_fd);
	close(listen_fd);
	return 0;
}

