@echo off
setlocal

set "HOST=192.168.1.18"
set "USER=javier"
set "PASS="

if "%~1"=="" (
    echo Uso: %~nx0 clave CLAVE [-ip IP] [-u USUARIO]
    echo Ejemplo: %~nx0 clave MiClave
    echo Ejemplo: %~nx0 -ip 192.168.1.18 -u javier clave MiClave
    echo La clave es obligatoria. IP por defecto: 192.168.1.18. Usuario por defecto: javier.
    exit /b 1
)

:parse_args
if "%~1"=="" goto args_done

if /i "%~1"=="-ip" (
    if "%~2"=="" goto missing_value
    set "HOST=%~2"
    shift
    shift
    goto parse_args
)

if /i "%~1"=="--ip" (
    if "%~2"=="" goto missing_value
    set "HOST=%~2"
    shift
    shift
    goto parse_args
)

if /i "%~1"=="-u" (
    if "%~2"=="" goto missing_value
    set "USER=%~2"
    shift
    shift
    goto parse_args
)

if /i "%~1"=="--usuario" (
    if "%~2"=="" goto missing_value
    set "USER=%~2"
    shift
    shift
    goto parse_args
)

if /i "%~1"=="clave" (
    if "%~2"=="" goto missing_value
    set "PASS=%~2"
    shift
    shift
    goto parse_args
)

if /i "%~1"=="password" (
    if "%~2"=="" goto missing_value
    set "PASS=%~2"
    shift
    shift
    goto parse_args
)

echo Parametro desconocido: %~1
echo Usa nombres de parametros: -ip, --ip, -u, --usuario, clave.
exit /b 1

:missing_value
echo Falta el valor para el parametro: %~1
exit /b 1

:args_done
if "%PASS%"=="" (
    echo Falta el parametro obligatorio: clave
    echo Uso: %~nx0 clave CLAVE [-ip IP] [-u USUARIO]
    exit /b 1
)

where plink >nul 2>nul
if errorlevel 1 (
    echo Error: no se encontro plink en el PATH.
    echo Instala PuTTY o anade plink.exe y pscp.exe al PATH.
    exit /b 1
)

where pscp >nul 2>nul
if errorlevel 1 (
    echo Error: no se encontro pscp en el PATH.
    echo Instala PuTTY o anade plink.exe y pscp.exe al PATH.
    exit /b 1
)

pushd "%~dp0" || exit /b 1

set "REMOTE_DIR=/home/%USER%/cookinggcode"

echo Creando directorio remoto %USER%@%HOST%:%REMOTE_DIR% ...
plink -batch -pw "%PASS%" "%USER%@%HOST%" "mkdir -p %REMOTE_DIR%"
if errorlevel 1 (
    popd
    echo Error creando el directorio remoto.
    exit /b 1
)

echo Copiando codigo al ordenador remoto ...
pscp -batch -pw "%PASS%" "main.cpp" "Makefile" "%USER%@%HOST%:%REMOTE_DIR%/"
if errorlevel 1 (
    popd
    echo Error copiando los archivos.
    exit /b 1
)

echo Compilando en Linux ...
plink -batch -pw "%PASS%" "%USER%@%HOST%" "cd %REMOTE_DIR% && make"
set "RESULT=%ERRORLEVEL%"

popd
exit /b %RESULT%
