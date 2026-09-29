# Verificacion de la version del 29-09-2026

Montaje real: TX COM50 (e0:72:a1:f9:72:00) y RX COM51 (14:c1:9f:51:4d:68), XIAO ESP32S3.
Firmware de ambos compilado/cargado y hash de flash verificado.
SHA256 del codigo de firmware: `63076e4b3060ea4a2a49071e23c99209b7c01228c5f5b4346255bd08ed0e3349`.

- 17 pruebas Python; DSP nativo C++; mapa y navegador Chrome: PASS.
- Referencia de amplitud: espera de 20 s + aproximadamente 15 s. Hubo
  resultados numericos provisionales antes de completar la referencia.
- TX detenido 5 s, reiniciado, reducido a 100 Hz y devuelto a 500 Hz:
  datos invalidados durante el fallo y recuperacion automatica, referencia 2
  conservada. No se produjo NEEDS_CALIBRATION.
- Fase continua de 310 s a 500 Hz: 1427/1447
  consultas con medicion valida (98.62 %).
  Los invalidos breves se recuperaron; no se perdio la referencia.
  Cero queue_drops, telemetry_drops y errores CRC en la captura continua.
- Intervalos entre resultados, durante esa fase: mediana 248.90 ms,
  percentil 90 256.12 ms. Window 125, hop 62, anillo 250 Hz.
- Capturador cerrado/reabierto: mismo boot y referencia; el contador RX
  paso de 246855 a 297413. No dependio de la GUI para medir.
  Se verifico la resincronizacion de la grafica tras mensajes USB pendientes.
- El usuario confirmo visualmente que el LED cambia de intensidad.
- Movimiento durante la referencia probado con senal sintetica: la actividad
  persiste, sin aprender un fondo de actividad. No es validacion con ratas.
- Mapa de hasta tres RX probado con datos sinteticos, incluyendo perdida de
  un enlace. Solo habia un RX fisico; falta validar la aproximacion espacial
  con varios receptores y posiciones de movimiento conocidas.

El diagnostico previo mostro borrado de la referencia al variar la tasa
recibida mas del 20 %. Ahora esos eventos solo reconstruyen filtros/ventanas.
Se midieron huecos RF de 20-30 ms aun con promedio proximo a 500 Hz; la nueva
interpolacion admite hasta 50 ms, limitada ademas por el cutoff. Los cortes
mayores siguen produciendo INVALID, conservando la referencia.

Esta prueba acotada verifica recuperacion y funcionamiento del montaje;
no constituye una prueba de estabilidad durante dias ni de deteccion de ratas.
Los registros temporales y dependencias de compilacion se eliminaron tras
extraer estos resultados para mantener compacta la carpeta.
