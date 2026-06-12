# Agent Notes

## Project Shape
- Single-file C++17 Linux utility: `main.cpp` builds to `cookinggcode-serial-scan` via the root `Makefile`.
- The program scans `/dev/serial/by-id`, `/dev/ttyUSB*`, and `/dev/ttyACM*`, opens each port, sends `$name`, and prints positive responses.
- This code depends on POSIX/Linux APIs (`termios`, `select`, `/dev`); do not assume it builds or runs natively on Windows.

## Commands
- Build: `make`
- Run after build: `./cookinggcode-serial-scan`
- Build and run: `make run`
- Clean generated binary: `make clean`
- Windows deploy helper uses named args: `deploy-linux.bat clave PASSWORD [-ip IP|--ip IP] [-u USER|--usuario USER]`; it copies `main.cpp` and `Makefile` to `/home/USER/cookinggcode` on the Linux host and runs `make` there. IP/user default to `192.168.1.18`/`javier`, password is required, and PuTTY `plink`/`pscp` must be on `PATH`.
- There is no test suite or CI config in this repo; use `make` as the focused verification step.

## Runtime Gotchas
- Serial-device access may require Linux `dialout` group membership: `sudo usermod -aG dialout "$USER"`, then log out and back in.
- Exit codes are runtime status, not build status: no USB serial ports returns `0`, at least one positive response returns `0`, and ports with no positive response return `1`.
