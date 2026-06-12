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

bool run_command_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << "No se pudo abrir el fichero: " << path << '\n';
        return false;
    }

    std::string line;
    int line_number = 0;
    bool success = true;
    while (std::getline(file, line)) {
        ++line_number;

        if (line.rfind("#", 0) == 0) {
            continue;
        } else if (line.rfind(":print", 0) == 0) {
            std::string text = line.substr(6);
            if (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
                text.erase(0, 1);
            }
            std::cout << text << '\n';
        } else if (!trim(line).empty()) {
            std::cerr << path << ':' << line_number << ": comando no reconocido: " << line << '\n';
            success = false;
        }
    }

    return success;
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

std::string lower_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
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
        return clean_name_response(response);
    }

    return std::nullopt;
}

bool scan_serial_ports() {
    const auto ports = find_usb_serial_ports();
    if (ports.empty()) {
        std::cout << "No se encontraron puertos serie USB (/dev/ttyUSB* o /dev/ttyACM*).\n";
        return true;
    }

    const std::vector<int> baud_rates = {115200, 250000, 230400, 57600, 38400, 19200, 9600};
    std::cout << "Puertos serie USB encontrados: " << ports.size() << '\n';

    std::vector<std::pair<std::string, std::string>> port_names;
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
                port_names.emplace_back(port, *name);
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
        for (const auto& [port, name] : port_names) {
            std::cout << '(' << port << ", " << name << ")\n";
        }
    }

    return !port_names.empty();
}

#endif

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
#else
    bool scan_ok = scan_serial_ports();
#endif

    if (argc == 2) {
        return run_command_file(argv[1]) ? 0 : 1;
    }

    return scan_ok ? 0 : 1;
}
