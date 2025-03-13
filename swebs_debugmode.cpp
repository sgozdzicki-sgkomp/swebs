#include <iostream>
#include <cstring>
#include <string>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sstream>
#include <fstream>
#include <sys/stat.h>
#include <getopt.h>

std::string BASE_DIRECTORY = "./www";
std::string PHP_FPM_SOCKET = "/run/php/php7.4-fpm.sock";
int PORT = 8080;

std::string get_file_content(const std::string &path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (file) {
        std::ostringstream contents;
        contents << file.rdbuf();
        file.close();
        return contents.str();
    }
    return "";
}

bool file_exists(const std::string &path) {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

std::string handle_php_request(const std::string &path, const std::string &request) {
    int sock;
    struct sockaddr_un server_addr;

    if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket failed");
        return "";
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, PHP_FPM_SOCKET.c_str(), sizeof(server_addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("connect failed");
        close(sock);
        return "";
    }

    std::string fcgi_request = request;

    if (write(sock, fcgi_request.c_str(), fcgi_request.size()) == -1) {
        perror("write failed");
        close(sock);
        return "";
    }

    char buffer[1024];
    std::string response;
    int bytes_read;
    while ((bytes_read = read(sock, buffer, sizeof(buffer))) > 0) {
        response.append(buffer, bytes_read);
    }

    close(sock);
    return response;
}

void handle_client(int client_socket) {
    char buffer[1024];
    read(client_socket, buffer, 1024);
    std::cout << "Received request:\n" << buffer << std::endl;

    std::istringstream request(buffer);
    std::string method, uri, version;
    request >> method >> uri >> version;

    std::cout << "Method: " << method << ", URI: " << uri << ", Version: " << version << std::endl;

    if (method == "GET") {
        std::string file_path = BASE_DIRECTORY + uri;
        if (file_path.back() == '/') {
            file_path += "index.html";
        }

        std::cout << "Requested file path: " << file_path << std::endl;

        if (file_path.find(".php") != std::string::npos) {
            std::string response = handle_php_request(file_path, buffer);
            write(client_socket, response.c_str(), response.size());
        } else if (file_exists(file_path)) {
            std::string content = get_file_content(file_path);
            std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: " + std::to_string(content.size()) + "\r\n"
                "Connection: close\r\n"
                "\r\n" + content;

            write(client_socket, response.c_str(), response.size());
        } else {
            std::cout << "File not found: " << file_path << std::endl;

            std::string not_found =
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Type: text/html\r\n"
                "Connection: close\r\n"
                "\r\n"
                "<!DOCTYPE html>"
                "<html>"
                "<head><title>404 Not Found</title></head>"
                "<body><h1>404 Not Found</h1></body>"
                "</html>";

            write(client_socket, not_found.c_str(), not_found.size());
        }
    } else {
        std::string not_implemented =
            "HTTP/1.1 501 Not Implemented\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<!DOCTYPE html>"
            "<html>"
            "<head><title>501 Not Implemented</title></head>"
            "<body><h1>501 Not Implemented</h1></body>"
            "</html>";

        write(client_socket, not_implemented.c_str(), not_implemented.size());
    }

    close(client_socket);
}

void print_usage(const char *program_name) {
    std::cerr << "Usage: " << program_name << " [options]\n"
              << "Options:\n"
              << "  -p, --port PORT            Port to listen on (default: 8080)\n"
              << "  -d, --directory DIRECTORY  Directory to serve files from (default: ./www)\n"
              << "  -s, --socket SOCKET        Path to PHP-FPM socket (default: /run/php/php7.4-fpm.sock)\n";
}

int main(int argc, char *argv[]) {
    int opt;
    struct option long_options[] = {
        {"port", required_argument, 0, 'p'},
        {"directory", required_argument, 0, 'd'},
        {"socket", required_argument, 0, 's'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "p:d:s:", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'p':
                PORT = std::stoi(optarg);
                break;
            case 'd':
                BASE_DIRECTORY = optarg;
                break;
            case 's':
                PHP_FPM_SOCKET = optarg;
                break;
            default:
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    int server_fd, client_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    // Tworzenie gniazda
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // Przygotowanie adresu serwera
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // Powiązanie gniazda z portem
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Nasłuchiwanie na połączenia
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    std::cout << "Server is listening on port " << PORT << std::endl;
    std::cout << "Serving files from directory: " << BASE_DIRECTORY << std::endl;
    std::cout << "Using PHP-FPM socket: " << PHP_FPM_SOCKET << std::endl;

    // Obsługa połączeń
    while (true) {
        if ((client_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept");
            close(server_fd);
            exit(EXIT_FAILURE);
        }

        handle_client(client_socket);
    }

    return 0;
}
//Some day comments will be translated to english, i promise
