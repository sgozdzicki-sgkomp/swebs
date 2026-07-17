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
#include <csignal>
#include <sys/wait.h>

// Globalne konfiguracje
std::string BASE_DIRECTORY = "./www";
std::string PHP_FPM_SOCKET = ""; // Zostanie automatycznie przypisany przy uruchomieniu lokalnym
int PORT = 8080;
int PROXY_PORT = -1;
std::string PROXY_HOST;
bool PROXY_ENABLED = false;
bool PROXY_PROTOCOL_V1 = false;
bool PROXY_PROTOCOL_V2 = false;
std::string SSL_CERT_PATH;
bool SSL_ENABLED = false;
int SSL_PORT = 8443;

// Zmienne do obsługi lokalnego PHP-FPM
pid_t php_fpm_pid = -1;
std::string TEMP_DIR = "/tmp/cpp_server_php";
std::string TEMP_SOCKET = TEMP_DIR + "/php.sock";
bool IS_LOCAL_PHP_FPM = false;

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

// Funkcja poszukująca binarnego pliku PHP-FPM w systemie
std::string find_php_fpm_binary() {
    std::vector<std::string> paths = {
        "php-fpm", // sprawdzi zmienną środowiskową PATH
        "/usr/sbin/php-fpm",
        "/usr/sbin/php-fpm8.3",
        "/usr/sbin/php-fpm8.2",
        "/usr/sbin/php-fpm8.1",
        "/usr/sbin/php-fpm8.0",
        "/usr/sbin/php-fpm7.4"
    };

    for (const auto &p : paths) {
        if (p[0] == '/') {
            struct stat buffer;
            if (stat(p.c_str(), &buffer) == 0 && (buffer.st_mode & S_IXUSR)) {
                return p;
            }
        } else {
            if (system(("which " + p + " > /dev/null 2>&1").c_str()) == 0) {
                return p;
            }
        }
    }
    return "";
}

// Czyszczenie zasobów po lokalnym PHP-FPM
void cleanup_local_php_fpm() {
    if (php_fpm_pid > 0) {
        std::cout << "\n[PHP-FPM] Zamykanie lokalnego procesu (PID: " << php_fpm_pid << ")..." << std::endl;
        kill(php_fpm_pid, SIGTERM);
        int status;
        waitpid(php_fpm_pid, &status, 0); // Uniknięcie procesów zombie
        php_fpm_pid = -1;
    }
    unlink(TEMP_SOCKET.c_str());
    unlink((TEMP_DIR + "/php-fpm.conf").c_str());
    unlink((TEMP_DIR + "/php-fpm.log").c_str());
    rmdir(TEMP_DIR.c_str());
}

// Obsługa sygnałów wyjścia (np. Ctrl+C)
void signal_handler(int signum) {
    if (IS_LOCAL_PHP_FPM) {
        cleanup_local_php_fpm();
    }
    exit(signum);
}

// Automatyczne generowanie konfiguracji i uruchomienie procesu PHP-FPM
bool start_local_php_fpm(const std::string &binary) {
    // Utworzenie katalogu tymczasowego w /tmp/
    mkdir(TEMP_DIR.c_str(), 0700);

    std::string conf_path = TEMP_DIR + "/php-fpm.conf";
    std::string log_path = TEMP_DIR + "/php-fpm.log";

    // Zapisywanie uproszczonej, bezpiecznej konfiguracji dla zalogowanego użytkownika
    std::ofstream conf(conf_path);
    if (!conf) {
        std::cerr << "Błąd: Nie można utworzyć konfiguracji PHP-FPM w " << conf_path << std::endl;
        return false;
    }
    conf << "[global]\n"
         << "error_log = " << log_path << "\n"
         << "daemonize = no\n\n" // Kontrolujemy proces bezpośrednio jako child
         << "[www]\n"
         << "listen = " << TEMP_SOCKET << "\n"
         << "pm = static\n"
         << "pm.max_children = 2\n"; // Minimum dla celów deweloperskich
    conf.close();

    std::cout << "[PHP-FPM] Uruchamianie lokalnej binarki: " << binary << std::endl;

    php_fpm_pid = fork();
    if (php_fpm_pid == 0) {
        // Proces potomny (Child)
        char* argv[] = {
            (char*)binary.c_str(),
            (char*)"-y",
            (char*)conf_path.c_str(),
            nullptr
        };
        execvp(argv[0], argv);
        perror("Błąd execvp przy starcie PHP-FPM");
        exit(EXIT_FAILURE);
    } else if (php_fpm_pid < 0) {
        perror("Fork nie powiódł się");
        return false;
    }

    // Czekanie max 3 sekundy na wygenerowanie pliku gniazda
    for (int i = 0; i < 30; ++i) {
        struct stat buffer;
        if (stat(TEMP_SOCKET.c_str(), &buffer) == 0) {
            PHP_FPM_SOCKET = TEMP_SOCKET;
            IS_LOCAL_PHP_FPM = true;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cerr << "Błąd: Przekroczono limit czasu oczekiwania na gniazdo PHP-FPM." << std::endl;
    cleanup_local_php_fpm();
    return false;
}

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

std::string build_fcgi_request(const std::string &script_path, const std::string &query_string, const std::string &method) {
    std::string request_body;

    request_body.append(make_fcgi_header(FCGI_BEGIN_REQUEST, 1, 8, 0));
    std::string begin_body(8, '\0');
    begin_body[0] = 0; 
    begin_body[1] = FCGI_RESPONDER; 
    begin_body[2] = 0; 
    request_body.append(begin_body);

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
    request_body.append(make_fcgi_header(FCGI_PARAMS, 1, 0, 0));
    request_body.append(make_fcgi_header(FCGI_STDIN, 1, 0, 0));

    return request_body;
}

std::string handle_php_request(const std::string &script_path, const std::string &query_string, const std::string &method) {
    int sock;
    struct sockaddr_un server_addr;

    if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        return "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nPHP Socket Error";
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, PHP_FPM_SOCKET.c_str(), sizeof(server_addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        close(sock);
        return "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nPHP-FPM Connection Failed.";
    }

    std::string fcgi_request = build_fcgi_request(script_path, query_string, method);
    if (write(sock, fcgi_request.data(), fcgi_request.size()) == -1) {
        close(sock);
        return "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nPHP-FPM Write Error";
    }

    std::string php_stdout;
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
        } else if (header.type == FCGI_END_REQUEST) {
            done = true;
        }
    }

    close(sock);

    std::string http_response;
    std::string status_line = "HTTP/1.1 200 OK\r\n";
    
    size_t status_pos = php_stdout.find("Status:");
    if (status_pos != std::string::npos) {
        size_t end_line = php_stdout.find("\r\n", status_pos);
        if (end_line != std::string::npos) {
            std::string status_val = php_stdout.substr(status_pos + 7, end_line - (status_pos + 7));
            status_val.erase(0, status_val.find_first_not_of(" \t")); 
            status_line = "HTTP/1.1 " + status_val + "\r\n";
            php_stdout.erase(status_pos, end_line + 2 - status_pos);
        }
    }

    http_response = status_line + php_stdout;
    return http_response;
}

void handle_client(int client_socket) {
    std::string raw_request;
    char buffer[1024];
    ssize_t bytes_read;

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

    std::istringstream request_stream(raw_request);
    std::string method, uri, version;
    request_stream >> method >> uri >> version;

    if (!is_safe_path(uri)) {
        std::string bad_req = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nDirectory Traversal Detected!";
        write(client_socket, bad_req.c_str(), bad_req.size());
        close(client_socket);
        return;
    }

    if (method == "GET") {
        std::string clean_uri = uri;
        std::string query_string = "";
        size_t q_pos = uri.find('?');
        if (q_pos != std::string::npos) {
            clean_uri = uri.substr(0, q_pos);
            query_string = uri.substr(q_pos + 1);
        }

        std::string file_path = BASE_DIRECTORY + clean_uri;
        if (file_path.back() == '/') {
            file_path += "index.php"; // Poprawka: domyślny plik indeksu na .php dla wygody testu
        }

        if (file_path.find(".php") != std::string::npos) {
            if (file_exists(file_path)) {
                char abs_path[PATH_MAX];
                std::string script_to_run = file_path;
                if (realpath(file_path.c_str(), abs_path) != nullptr) {
                    script_to_run = abs_path;
                }
                
                std::string response = handle_php_request(script_to_run, query_string, method);
                write(client_socket, response.c_str(), response.size());
            } else {
                std::string not_found = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n<h1>404 Not Found (Script missing)</h1>";
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
            std::string not_found = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n<h1>404 Not Found</h1>";
            write(client_socket, not_found.c_str(), not_found.size());
        }
    } else {
        std::string not_implemented = "HTTP/1.1 501 Not Implemented\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n<h1>501 Not Implemented</h1>";
        write(client_socket, not_implemented.c_str(), not_implemented.size());
    }

    close(client_socket);
}

int main(int argc, char *argv[]) {
    // Rejestracja obsługi sygnałów systemowych (sprzątanie po Ctrl+C)
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    int opt;
    int option_index = 0;
    struct option long_options[] = {
        {"port", required_argument, 0, 'p'},
        {"directory", required_argument, 0, 'd'},
        {"socket", required_argument, 0, 's'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "p:d:s:", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'p':
                PORT = std::stoi(optarg);
                break;
            case 'd':
                BASE_DIRECTORY = optarg;
                break;
            case 's':
                PHP_FPM_SOCKET = optarg; // Jeśli użytkownik wymusi zewnętrzne gniazdo
                break;
        }
    }

    // Jeśli użytkownik nie podał zewnętrznego gniazda, uruchom lokalną instancję PHP-FPM
    if (PHP_FPM_SOCKET.empty()) {
        std::string php_binary = find_php_fpm_binary();
        if (php_binary.empty()) {
            std::cerr << "Błąd: Nie znaleziono binarki php-fpm w systemie! Zainstaluj php-fpm lub wskaż gniazdo parametrem -s." << std::endl;
            exit(EXIT_FAILURE);
        }
        if (!start_local_php_fpm(php_binary)) {
            std::cerr << "Błąd: Nie udało się uruchomić lokalnego PHP-FPM." << std::endl;
            exit(EXIT_FAILURE);
        }
    }

    int server_fd, client_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        cleanup_local_php_fpm();
        exit(EXIT_FAILURE);
    }

    int opt_reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt_reuse, sizeof(opt_reuse));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        cleanup_local_php_fpm();
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        cleanup_local_php_fpm();
        exit(EXIT_FAILURE);
    }

    std::cout << "\n=============================================" << std::endl;
    std::cout << "Serwer działa na porcie: " << PORT << std::endl;
    std::cout << "Katalog główny WWW: " << BASE_DIRECTORY << std::endl;
    if (IS_LOCAL_PHP_FPM) {
        std::cout << "Uruchomiono PRYWATNY PHP-FPM (PID: " << php_fpm_pid << ")" << std::endl;
        std::cout << "Tymczasowe gniazdo: " << PHP_FPM_SOCKET << std::endl;
    } else {
        std::cout << "Połączono z zewnętrznym PHP-FPM: " << PHP_FPM_SOCKET << std::endl;
    }
    std::cout << "Wciśnij Ctrl+C, aby bezpiecznie wyłączyć serwer." << std::endl;
    std::cout << "=============================================\n" << std::endl;

    while (true) {
        if ((client_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            continue;
        }
        std::thread(handle_client, client_socket).detach();
    }

    close(server_fd);
    if (IS_LOCAL_PHP_FPM) cleanup_local_php_fpm();
    return 0;
}