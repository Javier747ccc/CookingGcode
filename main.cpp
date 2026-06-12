#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#ifndef _WIN32
#include <fcntl.h>
#endif
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <utility>
#ifndef _WIN32
#include <sys/select.h>
#include <termios.h>
#include <thread>
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
    std::string port;
    std::string name;
    int baud;
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

bool is_positive_response(const std::string& response) {
    const auto cleaned = trim(response);
    if (cleaned.empty()) {
        return false;
    }

    const auto lower = lower_copy(cleaned);
    return lower.find("error") == std::string::npos &&
           lower.find("unknown") == std::string::npos &&
           lower.find("invalid") == std::string::npos &&
           lower.find("unsupported") == std::string::npos &&
           lower.find("no reconocido") == std::string::npos;
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

    if (is_positive_response(response)) {
        const auto name = clean_name_response(response);
        std::cout << "Respuesta positiva en " << port << " @ " << baud << ": " << name << '\n';
        return name;
    }

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
                port_names.push_back({port, *name, baud});
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
#ifndef _WIN32
    const SerialPortInfo* selected_port = nullptr;
    std::vector<int> open_fds(ports.size(), -1);
#endif

    while (std::getline(file, line)) {
        ++line_number;
        const auto command = trim(line);

        if (command.empty() || command.rfind("#", 0) == 0) {
            continue;
        } else if (command.rfind(":print", 0) == 0) {
            std::cout << print_argument(command) << '\n';
        } else if (command.rfind(":use", 0) == 0) {
            const auto requested_name = lower_copy(trim(command.substr(4)));
            const SerialPortInfo* match = nullptr;
            for (const auto& port : ports) {
                if (lower_copy(port.name) == requested_name) {
                    match = &port;
                    break;
                }
            }

            if (match == nullptr) {
                std::cerr << path << ':' << line_number << ": puerto no encontrado para :use " << trim(command.substr(4)) << '\n';
                success = false;
                continue;
            }

#ifdef _WIN32
            std::cerr << path << ':' << line_number << ": el envio por puerto serie no esta soportado en Windows\n";
            success = false;
#else
            const auto port_index = static_cast<std::size_t>(match - ports.data());
            if (open_fds[port_index] < 0) {
                const auto fd = open_serial_port(*match);
                if (!fd) {
                    success = false;
                    selected_port = nullptr;
                    continue;
                }
                open_fds[port_index] = *fd;
            }

            selected_port = match;
            std::cout << "Usando " << selected_port->name << " en " << selected_port->port << '\n';
#endif
        } else if (command.rfind(":", 0) == 0) {
            std::cerr << path << ':' << line_number << ": comando no reconocido: " << command << '\n';
            success = false;
        } else {
#ifdef _WIN32
            std::cerr << path << ':' << line_number << ": el envio por puerto serie no esta soportado en Windows: " << command << '\n';
            success = false;
#else
            if (selected_port == nullptr) {
                std::cerr << path << ':' << line_number << ": no hay puerto seleccionado para enviar: " << command << '\n';
                success = false;
                continue;
            }

            const auto port_index = static_cast<std::size_t>(selected_port - ports.data());
            const int selected_fd = open_fds[port_index];
            if (selected_fd < 0) {
                std::cerr << selected_port->port << ": el puerto seleccionado no esta abierto\n";
                success = false;
                continue;
            }

            const auto serial_command = command + '\n';
            if (!write_all(selected_fd, serial_command)) {
                std::cerr << selected_port->port << ": no se pudo enviar: " << command << '\n';
                success = false;
                continue;
            }

            const auto response = trim(read_until_ok(selected_fd, std::chrono::milliseconds(5000)));
            if (!response.empty()) {
                std::cout << selected_port->name << ": " << response << '\n';
            }
            if (!response_has_ok(response)) {
                std::cerr << selected_port->port << ": no se recibio OK para: " << command << '\n';
                success = false;
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
