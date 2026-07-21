#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#ifndef _WIN32
#include <fcntl.h>
#endif
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <thread>
#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#endif
#include <vector>

namespace fs = std::filesystem;

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void handle_signal(int) {
    stop_requested = 1;
}

std::string trim(std::string text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string lower_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

struct SerialPortInfo {
    enum class Kind { Serial, Ethernet };

    Kind kind = Kind::Serial;
    std::string port;
    std::string name;
    int baud = 0;
    std::string ip;
    int tcp_port = 0;
};

struct ScanResult {
    bool ok;
    std::vector<SerialPortInfo> ports;
};

std::string print_argument(std::string line) {
    std::string text = line.substr(6);
    if (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.erase(0, 1);
    }
    return text;
}

#ifndef _WIN32

speed_t to_speed(int baud) {
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
#ifdef B250000
    case 250000: return B250000;
#endif
    default: return B115200;
    }
}

bool configure_serial(int fd, int baud) {
    termios tty{};
    if (tcgetattr(fd, &tty) != 0) {
        return false;
    }

    cfmakeraw(&tty);
    cfsetispeed(&tty, to_speed(baud));
    cfsetospeed(&tty, to_speed(baud));

    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        return false;
    }

    tcflush(fd, TCIOFLUSH);
    return true;
}

bool response_has_ok(const std::string& response) {
    const auto lower = lower_copy(response);
    std::size_t start = 0;
    while (start <= lower.size()) {
        const auto end = lower.find_first_of("\r\n", start);
        const auto line = trim(lower.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (line == "ok") {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

bool response_has_error(const std::string& response) {
    const auto lower = lower_copy(response);
    std::size_t start = 0;
    while (start <= lower.size()) {
        const auto end = lower.find_first_of("\r\n", start);
        const auto line = trim(lower.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (line.rfind("error", 0) == 0 ||
            line.rfind("alarm", 0) == 0 ||
            line.find("[msg:err") != std::string::npos) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

bool response_has_idle_status(const std::string& response) {
    return lower_copy(response).find("<idle") != std::string::npos;
}

bool response_has_alarm_status(const std::string& response) {
    return lower_copy(response).find("<alarm") != std::string::npos;
}

bool is_g1_command(const std::string& command) {
    const auto lower = lower_copy(command);
    if (lower.size() < 2 || lower[0] != 'g' || lower[1] != '1') {
        return false;
    }
    return lower.size() == 2 || std::isspace(static_cast<unsigned char>(lower[2]));
}

bool is_homing_command(const std::string& command) {
    const auto lower = lower_copy(command);
    return lower == "$h" || lower.rfind("$h", 0) == 0;
}

std::string clean_name_response(const std::string& response) {
    const auto cleaned = trim(response);

    const std::vector<std::string> prefixes = {"$/name=", "$name="};
    for (const auto& prefix : prefixes) {
        if (cleaned.rfind(prefix, 0) == 0) {
            auto value = cleaned.substr(prefix.size());
            const auto line_end = value.find_first_of("\r\n");
            if (line_end != std::string::npos) {
                value = value.substr(0, line_end);
            }
            return trim(value);
        }
    }

    return cleaned;
}

bool is_negative_response_line(const std::string& line) {
    const auto lower = lower_copy(line);
    return lower == "ok" ||
           lower.find("[cli]") != std::string::npos ||
           lower.find("error") != std::string::npos ||
           lower.find("unknown") != std::string::npos ||
           lower.find("invalid") != std::string::npos ||
           lower.find("unsupported") != std::string::npos ||
           lower.find("no reconocido") != std::string::npos ||
           lower.find("falta canal") != std::string::npos ||
           lower.find("usa un valor") != std::string::npos;
}

bool is_name_response_line(const std::string& line) {
    if (line.empty() || is_negative_response_line(line)) {
        return false;
    }

    return std::all_of(line.begin(), line.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '.';
    });
}

std::optional<std::string> extract_name_response(const std::string& response) {
    std::size_t start = 0;
    while (start <= response.size()) {
        const auto end = response.find_first_of("\r\n", start);
        const auto line = trim(response.substr(start, end == std::string::npos ? std::string::npos : end - start));
        const auto name = clean_name_response(line);
        if (is_name_response_line(name)) {
            return name;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    return std::nullopt;
}

std::string read_for(int fd, std::chrono::milliseconds timeout) {
    std::string response;
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (!stop_requested && std::chrono::steady_clock::now() < deadline) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(fd, &read_set);

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 100000;

        const int ready = select(fd + 1, &read_set, nullptr, nullptr, &tv);
        if (ready > 0 && FD_ISSET(fd, &read_set)) {
            char buffer[256];
            const ssize_t n = read(fd, buffer, sizeof(buffer));
            if (n > 0) {
                response.append(buffer, buffer + n);
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                break;
            }
        } else if (ready < 0 && errno != EINTR) {
            break;
        }
    }

    return response;
}

std::string read_until_ok(int fd, std::chrono::milliseconds timeout) {
    std::string response;
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (!stop_requested && std::chrono::steady_clock::now() < deadline) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(fd, &read_set);

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 100000;

        const int ready = select(fd + 1, &read_set, nullptr, nullptr, &tv);
        if (ready > 0 && FD_ISSET(fd, &read_set)) {
            char buffer[256];
            const ssize_t n = read(fd, buffer, sizeof(buffer));
            if (n > 0) {
                response.append(buffer, buffer + n);
                if (response_has_ok(response)) {
                    break;
                }
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                break;
            }
        } else if (ready < 0 && errno != EINTR) {
            break;
        }
    }

    return response;
}

std::vector<std::string> find_usb_serial_ports() {
    std::set<std::string> ports;

    const fs::path by_id = "/dev/serial/by-id";
    if (fs::exists(by_id)) {
        for (const auto& entry : fs::directory_iterator(by_id)) {
            std::error_code ec;
            const auto resolved = fs::canonical(entry.path(), ec);
            if (!ec) {
                ports.insert(resolved.string());
            }
        }
    }

    const std::vector<std::string> patterns = {"ttyUSB", "ttyACM"};
    for (const auto& entry : fs::directory_iterator("/dev")) {
        const auto name = entry.path().filename().string();
        for (const auto& prefix : patterns) {
            if (name.rfind(prefix, 0) == 0) {
                ports.insert(entry.path().string());
            }
        }
    }

    return {ports.begin(), ports.end()};
}

bool write_all(int fd, const std::string& text);

std::optional<std::string> query_port(const std::string& port, int baud) {
    const int fd = open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << port << " @ " << baud << ": no se pudo abrir: " << std::strerror(errno) << '\n';
        return std::nullopt;
    }

    const auto close_fd = [fd]() { close(fd); };

    if (!configure_serial(fd, baud)) {
        std::cerr << port << " @ " << baud << ": no se pudo configurar: " << std::strerror(errno) << '\n';
        close_fd();
        return std::nullopt;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    tcflush(fd, TCIOFLUSH);

    const std::string command = "$name\n";
    const ssize_t written = write(fd, command.data(), command.size());
    if (written != static_cast<ssize_t>(command.size())) {
        std::cerr << port << " @ " << baud << ": no se pudo escribir $name\n";
        close_fd();
        return std::nullopt;
    }

    const auto response = read_for(fd, std::chrono::milliseconds(1200));
    close_fd();

    if (const auto name = extract_name_response(response)) {
        std::cout << "Respuesta positiva en " << port << " @ " << baud << ": " << *name << '\n';
        return name;
    }

    return std::nullopt;
}

std::string endpoint_label(const SerialPortInfo& info) {
    if (info.kind == SerialPortInfo::Kind::Ethernet) {
        return info.ip + ':' + std::to_string(info.tcp_port);
    }
    return info.port;
}

std::optional<int> connect_ethernet(const std::string& ip, int tcp_port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << ip << ':' << tcp_port << ": no se pudo crear socket: " << std::strerror(errno) << '\n';
        return std::nullopt;
    }

    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(tcp_port));
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        std::cerr << ip << ':' << tcp_port << ": direccion IP no valida\n";
        close(fd);
        return std::nullopt;
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 && errno != EINPROGRESS) {
        std::cerr << ip << ':' << tcp_port << ": no se pudo conectar: " << std::strerror(errno) << '\n';
        close(fd);
        return std::nullopt;
    }

    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(fd, &write_set);

    timeval tv{};
    tv.tv_sec = 5;

    const int ready = select(fd + 1, nullptr, &write_set, nullptr, &tv);
    if (ready <= 0) {
        std::cerr << ip << ':' << tcp_port << ": timeout conectando\n";
        close(fd);
        return std::nullopt;
    }

    int socket_error = 0;
    socklen_t socket_error_size = sizeof(socket_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) != 0 || socket_error != 0) {
        std::cerr << ip << ':' << tcp_port << ": no se pudo conectar: "
                  << std::strerror(socket_error == 0 ? errno : socket_error) << '\n';
        close(fd);
        return std::nullopt;
    }

    return fd;
}

std::optional<std::string> query_ethernet_endpoint(const std::string& ip, int tcp_port) {
    const auto fd = connect_ethernet(ip, tcp_port);
    if (!fd) {
        return std::nullopt;
    }

    const std::string command = "$name\n";
    if (!write_all(*fd, command)) {
        std::cerr << ip << ':' << tcp_port << ": no se pudo escribir $name\n";
        close(*fd);
        return std::nullopt;
    }

    const auto response = read_for(*fd, std::chrono::milliseconds(1200));
    close(*fd);

    if (const auto name = extract_name_response(response)) {
        std::cout << "Respuesta positiva en " << ip << ':' << tcp_port << ": " << *name << '\n';
        return name;
    }

    std::cout << "Sin respuesta positiva en " << ip << ':' << tcp_port << '\n';
    return std::nullopt;
}

ScanResult scan_serial_ports() {
    const auto ports = find_usb_serial_ports();
    if (ports.empty()) {
        std::cout << "No se encontraron puertos serie USB (/dev/ttyUSB* o /dev/ttyACM*).\n";
        return {true, {}};
    }

    const std::vector<int> baud_rates = {115200, 250000, 230400, 57600, 38400, 19200, 9600};
    std::cout << "Puertos serie USB encontrados: " << ports.size() << '\n';

    std::vector<SerialPortInfo> port_names;
    for (const auto& port : ports) {
        if (stop_requested) {
            break;
        }

        bool port_positive = false;
        for (const int baud : baud_rates) {
            if (stop_requested) {
                break;
            }
            const auto name = query_port(port, baud);
            if (name) {
                port_names.push_back({SerialPortInfo::Kind::Serial, port, *name, baud});
                port_positive = true;
                break;
            }
        }

        if (!port_positive) {
            std::cout << "Sin respuesta positiva en " << port << '\n';
        }
    }

    if (!port_names.empty()) {
        std::cout << "Tabla de puertos encontrados:\n";
        for (const auto& info : port_names) {
            std::cout << '(' << info.port << ", " << info.name << ")\n";
        }
    }

    return {!port_names.empty(), port_names};
}

bool write_all(int fd, const std::string& text) {
    std::size_t total = 0;
    while (total < text.size()) {
        const ssize_t written = write(fd, text.data() + total, text.size() - total);
        if (written > 0) {
            total += static_cast<std::size_t>(written);
        } else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            return false;
        }
    }
    return true;
}

bool wait_until_complete(int fd, const std::string& port, const std::string& command, bool allow_alarm_status) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(30);
    while (!stop_requested && std::chrono::steady_clock::now() < deadline) {
        if (!write_all(fd, "?")) {
            std::cerr << port << ": no se pudo consultar estado tras: " << command << '\n';
            return false;
        }

        const auto response = read_for(fd, std::chrono::milliseconds(1000));
        if (response_has_error(response)) {
            std::cerr << port << ": el firmware devolvio error esperando fin de movimiento: " << command << '\n';
            return false;
        }
        if (response_has_idle_status(response)) {
            return true;
        }
        if (allow_alarm_status && response_has_alarm_status(response)) {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cerr << port << ": timeout esperando fin de movimiento: " << command << '\n';
    return false;
}

std::optional<int> open_serial_port(const SerialPortInfo& info) {
    const int fd = open(info.port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << info.port << " @ " << info.baud << ": no se pudo abrir: " << std::strerror(errno) << '\n';
        return std::nullopt;
    }

    if (!configure_serial(fd, info.baud)) {
        std::cerr << info.port << " @ " << info.baud << ": no se pudo configurar: " << std::strerror(errno) << '\n';
        close(fd);
        return std::nullopt;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    tcflush(fd, TCIOFLUSH);
    return fd;
}

std::optional<int> open_endpoint(const SerialPortInfo& info) {
    if (info.kind == SerialPortInfo::Kind::Ethernet) {
        return connect_ethernet(info.ip, info.tcp_port);
    }

    return open_serial_port(info);
}

#endif

bool run_command_file(const std::string& path, const std::vector<SerialPortInfo>& ports) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << "No se pudo abrir el fichero: " << path << '\n';
        return false;
    }

    std::string line;
    int line_number = 0;
    bool success = true;
    auto endpoints = ports;
#ifndef _WIN32
    std::optional<std::size_t> selected_port_index;
    std::vector<int> open_fds(endpoints.size(), -1);
#endif

    while (std::getline(file, line)) {
        ++line_number;
        const auto command = trim(line);

        if (command.empty() || command.rfind("#", 0) == 0) {
            continue;
        } else if (command.rfind(":print", 0) == 0) {
            std::cout << print_argument(command) << '\n';
        } else if (command.rfind(":delay", 0) == 0) {
            std::istringstream args(trim(command.substr(6)));
            int milliseconds = 0;
            std::string extra;
            if (!(args >> milliseconds) || (args >> extra) || milliseconds < 0) {
                std::cerr << path << ':' << line_number << ": uso: :delay MILISEGUNDOS\n";
                success = false;
                continue;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        } else if (command.rfind(":ether", 0) == 0) {
            std::istringstream args(trim(command.substr(6)));
            std::string ip;
            int tcp_port = 0;
            std::string extra;
            if (!(args >> ip >> tcp_port) || (args >> extra) || tcp_port <= 0 || tcp_port > 65535) {
                std::cerr << path << ':' << line_number << ": uso: :ether IP PUERTO\n";
                success = false;
                continue;
            }

#ifdef _WIN32
            std::cerr << path << ':' << line_number << ": el envio por ethernet no esta soportado en Windows\n";
            success = false;
#else
            const auto name = query_ethernet_endpoint(ip, tcp_port);
            if (!name) {
                success = false;
                continue;
            }

            endpoints.push_back({SerialPortInfo::Kind::Ethernet, "", *name, 0, ip, tcp_port});
            open_fds.push_back(-1);
            std::cout << "Puerto ethernet registrado: (" << ip << ':' << tcp_port << ", " << *name << ")\n";
#endif
        } else if (command.rfind(":use", 0) == 0) {
            const auto requested_name = lower_copy(trim(command.substr(4)));
            std::optional<std::size_t> match_index;
            for (std::size_t i = 0; i < endpoints.size(); ++i) {
                if (lower_copy(endpoints[i].name) == requested_name) {
                    match_index = i;
                    break;
                }
            }

            if (!match_index) {
                std::cerr << path << ':' << line_number << ": puerto no encontrado para :use " << trim(command.substr(4)) << '\n';
                success = false;
                continue;
            }

#ifdef _WIN32
            std::cerr << path << ':' << line_number << ": el envio por puerto no esta soportado en Windows\n";
            success = false;
#else
            const auto port_index = *match_index;
            if (open_fds[port_index] < 0) {
                const auto fd = open_endpoint(endpoints[port_index]);
                if (!fd) {
                    success = false;
                    selected_port_index.reset();
                    continue;
                }
                open_fds[port_index] = *fd;
            }

            selected_port_index = port_index;
            std::cout << "Usando " << endpoints[*selected_port_index].name << " en " << endpoint_label(endpoints[*selected_port_index]) << '\n';
#endif
        } else if (command.rfind(":", 0) == 0) {
            std::cerr << path << ':' << line_number << ": comando no reconocido: " << command << '\n';
            success = false;
        } else {
#ifdef _WIN32
            std::cerr << path << ':' << line_number << ": el envio por puerto serie no esta soportado en Windows: " << command << '\n';
            success = false;
#else
            if (!selected_port_index) {
                std::cerr << path << ':' << line_number << ": no hay puerto seleccionado para enviar: " << command << '\n';
                success = false;
                continue;
            }

            const auto port_index = *selected_port_index;
            const auto& selected_port = endpoints[port_index];
            const int selected_fd = open_fds[port_index];
            if (selected_fd < 0) {
                std::cerr << endpoint_label(selected_port) << ": el puerto seleccionado no esta abierto\n";
                success = false;
                continue;
            }

            if (selected_port.kind == SerialPortInfo::Kind::Serial) {
                tcflush(selected_fd, TCIFLUSH);
            }

            const auto serial_command = command + '\n';
            if (!write_all(selected_fd, serial_command)) {
                std::cerr << endpoint_label(selected_port) << ": no se pudo enviar: " << command << '\n';
                success = false;
                continue;
            }

            const auto response = trim(read_until_ok(selected_fd, std::chrono::seconds(60)));
            if (!response.empty()) {
                std::cout << selected_port.name << ": " << response << '\n';
            }
            if (response_has_error(response)) {
                std::cerr << endpoint_label(selected_port) << ": el firmware devolvio error para: " << command << '\n';
                success = false;
            } else if (!response_has_ok(response)) {
                std::cerr << endpoint_label(selected_port) << ": no se recibio OK para: " << command << '\n';
                success = false;
            } else if (is_g1_command(command)) {
                if (!wait_until_complete(selected_fd, endpoint_label(selected_port), command, false)) {
                    success = false;
                }
            } else if (is_homing_command(command)) {
                if (!wait_until_complete(selected_fd, endpoint_label(selected_port), command, true)) {
                    success = false;
                }
            }
#endif
        }
    }

#ifndef _WIN32
    for (const int fd : open_fds) {
        if (fd >= 0) {
            close(fd);
        }
    }
#endif

    return success;
}

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    if (argc > 2) {
        std::cerr << "Uso: " << argv[0] << " [fichero_comandos]\n";
        return 2;
    }

#ifdef _WIN32
    std::cerr << "El escaneo de puertos serie USB solo esta soportado en Linux.\n";
    bool scan_ok = false;
    std::vector<SerialPortInfo> serial_ports;
#else
    const auto scan_result = scan_serial_ports();
    bool scan_ok = scan_result.ok;
    const auto& serial_ports = scan_result.ports;
#endif

    if (argc == 2) {
        return run_command_file(argv[1], serial_ports) ? 0 : 1;
    }

    return scan_ok ? 0 : 1;
}
