# CookingGcode Serial Scan

Programa C++ para Ubuntu Linux que busca puertos serie USB (`/dev/ttyUSB*`, `/dev/ttyACM*` y `/dev/serial/by-id/*`), abre cada puerto, envía `$name` y muestra por pantalla las respuestas positivas.

## Compilar

```bash
make
```

## Ejecutar

```bash
./cookinggcode-serial-scan
```

Si el usuario no tiene permisos sobre los puertos serie, añadirlo al grupo `dialout` y volver a iniciar sesión:

```bash
sudo usermod -aG dialout "$USER"
```
