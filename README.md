# Rat CSI - XIAO ESP32S3

Un transmisor ESP-NOW y hasta tres receptores para observar actividad mediante
CSI. Cada receptor puede funcionar con batería o cargador USB, sin PC.
Es un detector experimental de actividad: no identifica ratas ni estima distancias.

## 1. Cargar las placas

Instala VS Code con la extensión PlatformIO. En PowerShell, desde esta carpeta:

```powershell
# Transmisor (cambia COMxx por su puerto).
.\firmware\build.ps1 -Environment tx -Port COMxx -Upload

# Repite con cada receptor: todos usan exactamente el mismo firmware.
.\firmware\build.ps1 -Environment rx -Port COMyy -Upload
```

Si PowerShell bloquea el script, antepone
`powershell -ExecutionPolicy Bypass -File` al comando del script.
No hay que configurar WiFi ni password. El canal debe coincidir en todos los nodos.
El script usa temporalmente R: para evitar los problemas de rutas con espacios;
no modifica ni copia otro proyecto. Las dependencias se descargan automáticamente.
La primera compilación necesita Internet y puede tardar varios minutos.

## 2. Medir con o sin PC

Enciende el TX y los RX en sus posiciones definitivas. Tras recibir muestras
suficientes para la primera ventana, ya hay resultados **PROVISIONAL**.
Cada RX espera 20 segundos y obtiene una referencia de amplitud durante unos
15 segundos de datos utilizables. BOOT pulsado 1,5 segundos repite esa espera.
No mantengas BOOT al encender: ese gesto entra en el cargador de firmware.

**Se elimino la referencia de actividad de 45 segundos.** El score expresa
la dispersion robusta de amplitud relativa en porcentaje, sin restar actividad
aprendida ni dividir por la variabilidad de un fondo. Movimiento presente al
calibrar no se aprende como ausencia de actividad. La normalizacion de amplitud
sigue dependiendo del montaje; repitela al mover las placas.

Por defecto: `window=125` muestras (~500 ms) y `hop=62` (~248 ms entre
resultados), sobre la senal filtrada a 250 Hz. La captura RF sigue a 500 Hz.
La version 3 migra los antiguos window/hop y umbrales al cargar configuraciones
v1/v2. `enter_score=4` y `exit_score=2.5` son umbrales experimentales en la nueva
escala, no los antiguos z-scores ni probabilidades de detectar una rata.

El LED usa el maximo de score observado: un nuevo maximo redefine el 100% PWM;
los valores menores quedan proporcionalmente mas tenues. Un minimo de escala
impide amplificar ilimitadamente ruido casi nulo. El maximo se reinicia al
encender/referenciar. No se usa esta normalizacion del LED para el mapa.

| Indicacion del LED | Significado |
| --- | --- |
| Destello breve cada segundo | Espera de 20 s; la GUI ya puede mostrar valores provisionales |
| Brillo que sube y baja | Obteniendo referencia de amplitud |
| Dos destellos cada dos segundos | Datos invalidos: no deben interpretarse como ausencia de movimiento |
| Brillo proporcional | Actividad relativa al maximo observado |
| Brillo tenue fijo en TX | Transmisor habilitado |

Los cortes, bajadas de tasa y desbordamientos vacian las ventanas afectadas,
pero conservan la referencia. El procesamiento se recupera automaticamente al
volver datos suficientes. Un cambio real del formato CSI requiere una nueva
referencia de amplitud, que se inicia automaticamente. Los diagnosticos muestran
el ultimo motivo de recuperacion y un contador, sin ocultar los fallos de enlace.
El remuestreo interpola separaciones de hasta 50 ms (o medio periodo del cutoff,
si es menor). Huecos mayores reinician la ventana, conservando la referencia.

## 3. Interfaz web opcional

Instala Python 3.10 o posterior y, una vez, la dependencia:

```powershell
python -m pip install -r host/requirements.txt
```

Conecta los receptores por USB y ejecuta:

```powershell
python start.py
```

Detecta los puertos automáticamente y abre **http://127.0.0.1:8766/**.
El TX puede permanecer alimentado en otro lugar. Conectarlo al PC solo hace
falta para cargarlo o cambiar su configuracion. Cierra otros monitores serie.
RX1/RX2/RX3 son etiquetas del PC, ordenadas por identidad de placa; no son firmwares distintos.
Si prefieres indicar los puertos:

```powershell
python host/capture.py --rx1 COMxx --rx2 COMyy --open
```

Puedes usar un solo RX omitiendo `--rx2`; el tercer RX se indica con `--rx3 COMzz`. El botón de calibración también espera
20 s. La interfaz guarda `scores.csv`, eventos y marcas en `captures/`.
`python start.py --raw` incluye CSI crudo; normalmente no hace falta.
Cerrar la interfaz/capturador no detiene el sensado autónomo.

## Ajustes y archivos

Los parámetros están al principio de `firmware/common/experiment_config.h`.
`led_full_scale` fija el minimo inicial de la escala automatica del LED.
`window`, `hop` y otros parámetros también se cambian desde los ajustes de la GUI.
Los valores guardados en la placa prevalecen sobre los defaults del código;
usa **Restaurar defaults** y recalibra si quieres aplicar los valores compilados.
La tasa habitual del transmisor es 500 paquetes/s.

- `firmware/`: dos entornos, `tx` y `rx`, con procesamiento compartido.
- `host/` y `start.py`: interfaz, conexión USB y registro opcionales.
- `tests/`: comprobaciones del protocolo, reconexion, GUI, DSP y LED.

Las compilaciones, capturas y dependencias generadas están excluidas de Git.
No hay que subir `.pio/`, `managed_components/`, `sdkconfig.rx/tx` ni `captures/`.
Se conserva la licencia MIT y la atribución del proyecto original
[skizzophrenic/Cardputer-CSI-Human-Detector](https://github.com/skizzophrenic/Cardputer-CSI-Human-Detector).

## Mapa experimental

La GUI permite arrastrar TX/RX o introducir sus coordenadas en metros. Guarda
el montaje para confirmar las posiciones: los valores iniciales son ejemplos.
Admite hasta tres receptores, todos con el mismo firmware. Los halos y enlaces
muestran respuestas simultaneas, sin inferir cuantos animales hay.

El centro ponderado es opcional: combina las posiciones de los RX segun su
actividad, con factores de sensibilidad ajustables. Necesita al menos dos
receptores validos y referenciados; con uno solo muestra el enlace. Las distancias
al centro son geometricas sobre esa heuristica, no medidas de alcance RF. No
resuelve dos animales separados: ambos enlaces pueden responder sin que el
centro coincida con un animal. Validar con posiciones conocidas antes de usarlo
como estimador espacial. Las posiciones quedan guardadas en el navegador y,
al editarlas, en los metadatos de la sesion.

## Pruebas

`python -m unittest discover -s tests` verifica el host. `tests/native.ps1`
(MSVC Build Tools) prueba el DSP real, incluidos movimiento durante referencia,
salida provisional, 248 ms, cambio de tasa, cortes, reinicio del TX y LED.
Para la GUI: entra en `tests`, ejecuta `npm install` y despues `npm test`.
`node tests/test_map.cjs` verifica la heuristica y la exclusion de datos invalidos.
Estas dependencias de prueba no son necesarias para usar la interfaz.
La comprobacion con las placas y sus limites estan en [tests/verification.md](tests/verification.md).

Pin y polaridad del LED: [Seeed](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/).
