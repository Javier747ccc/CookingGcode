#pruebas de Cooking
:print Hola mundo
:use cooking1
# Retorno carro X
$x
$hx
g1 y30 f500
:use Cooking2
# Retorno carro Z,A,B
$x
$ha
$hz
$hb
# Movimientos a Pos 1
g1 z2 f100
g1 b10.5 f700
g1 a6.5 f100
# movimiento Garra
g1 a0 f100
g1 z10 f100
# Movimiento a Reposo
:use Cooking1
g1 y0 f500
