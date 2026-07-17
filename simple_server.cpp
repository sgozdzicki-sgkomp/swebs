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
#include <vector>
#include <climits>

// Globalne konfiguracje
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

// --- STRUKTURY I STAŁE FASTCGI ---
constexpr uint8_t FCGI_VERSION_1 = 1;
constexpr uint8_t FCGI_BEGIN_REQUEST = 1;
constexpr uint8_t FCGI_PARAMS = 4;
constexpr uint8_t FCGI_STDIN = 5;
constexpr uint8_t FCGI_STDOUT = 6;
constexpr uint8_t FCGI_STDERR = 7;
constexpr uint8_t FCGI_END_REQUEST = 3;
constexpr uint16_t FCGI_RESPONDER = 1;

#pragma pack(push, 1)
struct FCGI_Header {
    uint8_t version;
    uint8_t type;
    uint8_t requestIdB1;
    uint8_t requestIdB0;
    uint8_t contentLengthB1;
    uint8_t contentLengthB0;
    uint8_t paddingLength;
    uint8_t reserved;
};
#pragma pack(pop)

// Pomocnicze funkcje budowania rekordów FastCGI
std::string make_fcgi_header(uint8_t type, uint16_t reqId, uint16_t contentLen, uint8_t paddingLen) {
    std::string header(8, '\0');
    header[0] = FCGI_VERSION_1;
    header[1] = type;
    header[2] = static_cast<char>((reqId >> 8) & 0xFF);
    header[3] = static_cast<char>(reqId & 0xFF);
    header[4] = static_cast<char>((contentLen >> 8) & 0xFF);
    header[5] = static_cast<char>(contentLen & 0xFF);
    header[6] = static_cast<char>(paddingLen);
    header[7] = 0;
    return header;
}

std::string make_fcgi_param(const std::string &key, const std::string &val) {
    std::string param;
    size_t klen = key.size();
    size_t vlen = val.size();

    if (klen < 128) {
        param.push_back(static_cast<char>(klen));
    } else {
        param.push_back(static_cast<char>((klen >> 24) | 0x80));
        param.push_back(static_cast<char>((klen >> 16) & 0xFF));
        param.push_back(static_cast<char>((klen >> 8) & 0xFF));
        param.push_back(static_cast<char>(klen & 0xFF));
    }

    if (vlen < 128) {
        param.push_back(static_cast<char>(vlen));
    } else {
        param.push_back(static_cast<char>((vlen >> 24) | 0x80));
        param.push_back(static_cast<char>((vlen >> 16) & 0xFF));
        param.push_back(static_cast<char>((vlen >> 8) & 0xFF));
        param.push_back(static_cast<char>(vlen & 0xFF));
    }

    param.append(key);
    param.append(val);
    return param;
}

// Funkcja zabezpieczająca przed Directory Traversal
bool is_safe_path(const std::string &path) {
    return path.find("..") == std::string::npos;
}

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

// Przygotowanie pełnego binarnego żądania FastCGI
std::string build_fcgi_request(const std::string &script_path, const std::string &query_string, const std::string &method) {
    std::string request_body;

    // 1. Rekord początkowy: FCGI_BEGIN_REQUEST
    request_body.append(make_fcgi_header(FCGI_BEGIN_REQUEST, 1, 8, 0));
    std::string begin_body(8, '\0');
    begin_body[0] = 0; // roleB1
    begin_body[1] = FCGI_RESPONDER; // roleB0 (1)
    begin_body[2] = 0; // flags (0 oznacza zamknięcie połączenia po zakończeniu)
    request_body.append(begin_body);

    // 2. Rekord zmiennych środowiskowych: FCGI_PARAMS
    std::vector<std::pair<std::string, std::string>> params = {
        {"SCRIPT_FILENAME", script_path},
        {"REQUEST_METHOD", method},
        {"QUERY_STRING", query_string},
        {"REQUEST_URI", script_path + (query_string.empty() ? "" : "?" + query_string)},
        {"DOCUMENT_ROOT", BASE_DIRECTORY},
        {"GATEWAY_INTERFACE", "CGI/1.1"},
        {"SERVER_SOFTWARE", "CustomCppServer/1.1"}
    };

    std::string params_payload;
    for (const auto &p : params) {
        params_payload.append(make_fcgi_param(p.first, p.second));
    }

    request_body.append(make_fcgi_header(FCGI_PARAMS, 1, params_payload.size(), 0));
    request_body.append(params_payload);

    // Pusty pakiet PARAMS zamyka sekcję przesyłania parametrów
    request_body.append(make_fcgi_header(FCGI_PARAMS, 1, 0, 0));

    // Pusty pakiet STDIN (dla żądań typu GET brak danych wejściowych)
    request_body.append(make_fcgi_header(FCGI_STDIN, 1, 0, 0));

    return request_body;
}

// Poprawna obsługa PHP-FPM za pomocą binarnego FastCGI
std::string handle_php_request(const std::string &script_path, const std::string &query_string, const std::string &method) {
    int sock;
    struct sockaddr_un server_addr;

    if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("PHP-FPM: socket creation failed");
        return "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nPHP-FPM Socket Error";
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, PHP_FPM_SOCKET.c_str(), sizeof(server_addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("PHP-FPM: connect failed");
        close(sock);
        return "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nPHP-FPM Connection Failed. Is the service running?";
    }

    // Budujemy i wysyłamy żądanie FastCGI
    std::string fcgi_request = build_fcgi_request(script_path, query_string, method);
    if (write(sock, fcgi_request.data(), fcgi_request.size()) == -1) {
        perror("PHP-FPM: write failed");
        close(sock);
        return "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nPHP-FPM Write Error";
    }

    // Odbieramy i dekodujemy strumień wyjściowy FastCGI
    std::string php_stdout;
    std::string php_stderr;
    bool done = false;

    while (!done) {
        FCGI_Header header;
        ssize_t n = read(sock, &header, sizeof(header));
        if (n <= 0) break;

        uint16_t contentLength = (header.contentLengthB1 << 8) | header.contentLengthB0;
        uint8_t paddingLength = header.paddingLength;

        std::vector<char> content(contentLength);
        if (contentLength > 0) {
            ssize_t bytes_read = 0;
            while (bytes_read < contentLength) {
                ssize_t r = read(sock, content.data() + bytes_read, contentLength - bytes_read);
                if (r <= 0) break;
                bytes_read += r;
            }
        }

        if (paddingLength > 0) {
            std::vector<char> padding(paddingLength);
            read(sock, padding.data(), paddingLength);
        }

        if (header.type == FCGI_STDOUT) {
            php_stdout.append(content.data(), contentLength);
        } else if (header.type == FCGI_STDERR) {
            php_stderr.append(content.data(), contentLength);
        } else if (header.type == FCGI_END_REQUEST) {
            done = true;
        }
    }

    close(sock);

    if (!php_stderr.empty()) {
        std::cerr << "PHP Error: " << php_stderr << std::endl;
    }

    // Mapowanie nagłówka "Status: XXX" generowanego przez PHP na poprawną linię statusu HTTP
    std::string http_response;
    std::string status_line = "HTTP/1.1 200 OK\r\n";
    
    size_t status_pos = php_stdout.find("Status:");
    if (status_pos != std::string::npos) {
        size_t end_line = php_stdout.find("\r\n", status_pos);
        if (end_line != std::string::npos) {
            std::string status_val = php_stdout.substr(status_pos + 7, end_line - (status_pos + 7));
            status_val.erase(0, status_val.find_first_not_of(" \t")); // trim left
            status_line = "HTTP/1.1 " + status_val + "\r\n";
            php_stdout.erase(status_pos, end_line + 2 - status_pos); // Usuń Status z nagłówków wyjściowych
        }
    }

    http_response = status_line + php_stdout;
    return http_response;
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

    // Uproszczone przekazywanie jednoetapowe (blokujące)
    while ((bytes_read = read(client_socket, buffer, sizeof(buffer))) > 0) {
        write(proxy_socket, buffer, bytes_read);
    }

    while ((bytes_read = read(proxy_socket, buffer, sizeof(buffer))) > 0) {
        write(client_socket, buffer, bytes_read);
    }

    close(proxy_socket);
}

void handle_client(int client_socket) {
    std::string raw_request;
    char buffer[1024];
    ssize_t bytes_read;

    // Bezpieczne wczytywanie nagłówka do momentu znalezienia podwójnego znaku nowej linii \r\n\r\n
    while ((bytes_read = read(client_socket, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        raw_request.append(buffer, bytes_read);
        if (raw_request.find("\r\n\r\n") != std::string::npos || raw_request.size() > 4096) {
            break;
        }
    }

    if (raw_request.empty()) {
        close(client_socket);
        return;
    }

    std::cout << "Received request:\n" << raw_request << std::endl;

    std::istringstream request_stream(raw_request);
    std::string method, uri, version;
    request_stream >> method >> uri >> version;

    std::cout << "Method: " << method << ", URI: " << uri << ", Version: " << version << std::endl;

    if (!is_safe_path(uri)) {
        std::string bad_req = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nDirectory Traversal Detected!";
        write(client_socket, bad_req.c_str(), bad_req.size());
        close(client_socket);
        return;
    }

    if (PROXY_ENABLED) {
        handle_proxy(client_socket);
        close(client_socket);
        return;
    }

    if (method == "GET") {
        // Oddzielenie ścieżki pliku od parametrów GET (Query String)
        std::string clean_uri = uri;
        std::string query_string = "";
        size_t q_pos = uri.find('?');
        if (q_pos != std::string::npos) {
            clean_uri = uri.substr(0, q_pos);
            query_string = uri.substr(q_pos + 1);
        }

        std::string file_path = BASE_DIRECTORY + clean_uri;
        if (file_path.back() == '/') {
            file_path += "index.html";
        }

        std::cout << "Requested file path: " << file_path << std::endl;

        if (file_path.find(".php") != std::string::npos) {
            if (file_exists(file_path)) {
                // Konwersja ścieżki względnej na absolutną dla PHP-FPM (wymagane przez SCRIPT_FILENAME)
                char abs_path[PATH_MAX];
                std::string script_to_run = file_path;
                if (realpath(file_path.c_str(), abs_path) != nullptr) {
                    script_to_run = abs_path;
                }
                
                std::string response = handle_php_request(script_to_run, query_string, method);
                write(client_socket, response.c_str(), response.size());
            } else {
                std::string not_found = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n<h1>404 Not Found (PHP script missing)</h1>";
                write(client_socket, not_found.c_str(), not_found.size());
            }
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
    int option_index = 0;
    struct option long_options[] = {
        {"port", required_argument, 0, 'p'},
        {"directory", required_argument, 0, 'd'},
        {"socket", required_argument, 0, 's'},
        {"proxy-port", required_argument, 0, 'x'},
        {"proxy-host", required_argument, 0, 'h'},
        {"proxy-protocol-v1", no_argument, 0, 1001},
        {"proxy-protocol-v2", no_argument, 0, 1002},
        {"ssl-cert", required_argument, 0, 1003},
        {"ssl-port", required_argument, 0, 1004},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "p:d:s:x:h:", long_options, &option_index)) != -1) {
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
            case 1001:
                PROXY_PROTOCOL_V1 = true;
                break;
            case 1002:
                PROXY_PROTOCOL_V2 = true;
                break;
            case 1003:
                SSL_CERT_PATH = optarg;
                SSL_ENABLED = true;
                break;
            case 1004:
                SSL_PORT = std::stoi(optarg);
                break;
            default:
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    int server_fd, client_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // POPRAWKA: Dodanie SO_REUSEADDR zapobiega błędom "Address already in use" przy restartach
    int opt_reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt_reuse, sizeof(opt_reuse)) < 0) {
        perror("setsockopt SO_REUSEADDR failed");
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    std::cout << "Server is listening on port " << PORT << std::endl;
    std::cout << "Serving files from directory: " << BASE_DIRECTORY << std::endl;
    std::cout << "Using PHP-FPM socket: " << PHP_FPM_SOCKET << std::endl;

    while (true) {
        if ((client_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept");
            continue; // Nie przerywaj działania całego serwera z powodu jednego błędu połączenia
        }

        std::thread(handle_client, client_socket).detach();
    }

    close(server_fd);
    return 0;
}