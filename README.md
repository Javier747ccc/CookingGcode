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

También puede ejecutar un fichero de comandos:

```bash
./cookinggcode-serial-scan prueba.gcode
```

En los ficheros de comandos, `:ether IP PUERTO` registra un puerto Ethernet. El programa conecta, envía `$name` y añade la respuesta a la tabla de puertos disponibles para usarla después con `:use`, igual que con los puertos serie:

```gcode
:ether 192.168.1.7 20108
:use cooking1
:delay 2000
g1 x20 f800
```

`:delay MILISEGUNDOS` pausa la ejecucion del fichero durante el tiempo indicado.

Si el usuario no tiene permisos sobre los puertos serie, añadirlo al grupo `dialout` y volver a iniciar sesión:

```bash
sudo usermod -aG dialout "$USER"
```

Para subir y compilar la aplicacion usar el script deploy-config.bat

