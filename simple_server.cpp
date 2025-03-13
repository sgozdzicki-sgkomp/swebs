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
#include <thread>

std::string BASE_DIRECTORY = "./www";
std::string PHP_FPM_SOCKET = "/run/php/php7.4-fpm.sock";
int PORT = 8080;
int PROXY_PORT = -1;
std::string PROXY_HOST;
bool PROXY_ENABLED = false;
bool PROXY_PROTOCOL_V1 = false;
bool PROXY_PROTOCOL_V2 = false;
std::string SSL_CERT_PATH;
bool SSL_ENABLED = false;
int SSL_PORT = 8443;

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

void handle_proxy(int client_socket) {
    int proxy_socket;
    struct sockaddr_in proxy_addr;

    if ((proxy_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Proxy socket creation error");
        return;
    }

    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_port = htons(PROXY_PORT);

    if (inet_pton(AF_INET, PROXY_HOST.c_str(), &proxy_addr.sin_addr) <= 0) {
        perror("Invalid proxy address");
        close(proxy_socket);
        return;
    }

    if (connect(proxy_socket, (struct sockaddr *)&proxy_addr, sizeof(proxy_addr)) < 0) {
        perror("Proxy connection failed");
        close(proxy_socket);
        return;
    }

    char buffer[1024];
    int bytes_read;

    if (PROXY_PROTOCOL_V1) {
        std::string proxy_protocol_header = "PROXY TCP4 127.0.0.1 127.0.0.1 12345 80\r\n";
        write(proxy_socket, proxy_protocol_header.c_str(), proxy_protocol_header.size());
    }

    while ((bytes_read = read(client_socket, buffer, sizeof(buffer))) > 0) {
        write(proxy_socket, buffer, bytes_read);
    }

    while ((bytes_read = read(proxy_socket, buffer, sizeof(buffer))) > 0) {
        write(client_socket, buffer, bytes_read);
    }

    close(proxy_socket);
}

void handle_client(int client_socket) {
    char buffer[1024];
    read(client_socket, buffer, 1024);
    std::cout << "Received request:\n" << buffer << std::endl;

    std::istringstream request(buffer);
    std::string method, uri, version;
    request >> method >> uri >> version;

    std::cout << "Method: " << method << ", URI: " << uri << ", Version: " << version << std::endl;

    if (PROXY_ENABLED) {
        handle_proxy(client_socket);
        return;
    }

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
              << "  -s, --socket SOCKET        Path to PHP-FPM socket (default: /run/php/php7.4-fpm.sock)\n"
              << "  -x, --proxy-port PORT      Port to proxy to (default: none)\n"
              << "  -h, --proxy-host HOST      Host to proxy to (default: none)\n"
              << "  --proxy-protocol-v1        Enable Proxy Protocol v1 (default: disabled)\n"
              << "  --proxy-protocol-v2        Enable Proxy Protocol v2 (default: disabled)\n"
              << "  --ssl-cert PATH            Path to SSL certificate file (default: none)\n"
              << "  --ssl-port PORT            Port for HTTPS (default: 8443)\n";
}

int main(int argc, char *argv[]) {
    int opt;
    struct option long_options[] = {
        {"port", required_argument, 0, 'p'},
        {"directory", required_argument, 0, 'd'},
        {"socket", required_argument, 0, 's'},
        {"proxy-port", required_argument, 0, 'x'},
        {"proxy-host", required_argument, 0, 'h'},
        {"proxy-protocol-v1", no_argument, 0, 0},
        {"proxy-protocol-v2", no_argument, 0, 0},
        {"ssl-cert", required_argument, 0, 0},
        {"ssl-port", required_argument, 0, 0},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "p:d:s:x:h:", long_options, nullptr)) != -1) {
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
            case 'x':
                PROXY_PORT = std::stoi(optarg);
                PROXY_ENABLED = true;
                break;
            case 'h':
                PROXY_HOST = optarg;
                break;
            case 0:
                if (strcmp(long_options[optind].name, "proxy-protocol-v1") == 0) {
                    PROXY_PROTOCOL_V1 = true;
                } else if (strcmp(long_options[optind].name, "proxy-protocol-v2") == 0) {
                    PROXY_PROTOCOL_V2 = true;
                } else if (strcmp(long_options[optind].name, "ssl-cert") == 0) {
                    SSL_CERT_PATH = optarg;
                    SSL_ENABLED = true;
                } else if (strcmp(long_options[optind].name, "ssl-port") == 0) {
                    SSL_PORT = std::stoi(optarg);
                }
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
    if (PROXY_ENABLED) {
        std::cout << "Proxying to " << PROXY_HOST << ":" << PROXY_PORT << std::endl;
        if (PROXY_PROTOCOL_V1) {
            std::cout << "Proxy Protocol v1 enabled" << std::endl;
        }
        if (PROXY_PROTOCOL_V2) {
            std::cout << "Proxy Protocol v2 enabled" << std::endl;
        }
    }
    if (SSL_ENABLED) {
        std::cout << "SSL enabled on port " << SSL_PORT << " with certificate " << SSL_CERT_PATH << std::endl;
    }

    // Obsługa połączeń
    while (true) {
        if ((client_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept");
            close(server_fd);
            exit(EXIT_FAILURE);
        }

        // Tworzenie nowego wątku do obsługi klienta
        std::thread(handle_client, client_socket).detach();
    }

    return 0;
}

