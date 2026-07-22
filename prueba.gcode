#pruebas de Cooking
:print Hola mundo
:ether 192.168.1.7 20108
:use cooking1
$hx
$x
g1 x20 f800
:use Cooking2
$hz
$ha
$x
:use Claw
r 270
:delay 2000
r 200
:use Cooking2
g1 a6.5 f100
g1 a0 f100
$hb
g1 b10 f700
g1 z10 f300
:use Cooking1
g1 y20 f500
g1 y0 f500

