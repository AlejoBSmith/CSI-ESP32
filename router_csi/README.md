# Router CSI — versión experimental

Variante para usar un router Wi-Fi como transmisor y uno o varios Seeed Studio
XIAO ESP32S3 como receptores CSI. El montaje probado usa un TP-Link TL-WR1512X V1
con firmware oficial y dos receptores. No necesita Internet ni un XIAO transmisor.

El firmware calcula actividad en cada receptor y controla su LED. La interfaz
web opcional muestra las respuestas individuales y un índice conjunto calculado
en la PC. Es un detector experimental de actividad: todavía no se ha validado
con ratas, no identifica especies y no mide distancias.

La variante ESP-NOW anterior permanece en la raíz del repositorio. Los comandos
de esta página se ejecutan **dentro de `router_csi`**.

## 1. Descargar y cargar el firmware

Instala Git, Python 3.10 o posterior y VS Code con la extensión PlatformIO.
Desde PowerShell:

```powershell
git clone --branch experimental/router-csi https://github.com/AlejoBSmith/CSI-ESP32.git
cd CSI-ESP32\router_csi

# Compilar; la primera vez necesita Internet para descargar las dependencias.
powershell -ExecutionPolicy Bypass -File .\firmware\build.ps1

# Cargar; cambia COMxx por el puerto del XIAO.
powershell -ExecutionPolicy Bypass -File .\firmware\build.ps1 -Port COMxx -Upload
```

Repite la carga en cada receptor: **todos usan el mismo firmware `router_rx`**.
No necesitas modificar un identificador en el código. El script crea un alias
S: temporal para evitar problemas de ESP-IDF con espacios y rutas largas.
Debe estar disponible esa letra de unidad. Este script está preparado para Windows.

## 2. Abrir la interfaz y conectar cada receptor al AP

```powershell
python -m pip install -r host/requirements.txt
python start.py
```

Se abre **http://127.0.0.1:8767/**. Cierra otros monitores que estén usando los
puertos serie. El lanzador reconoce solo el firmware de esta variante y mantiene
los alias RX1/RX2/RX3 por identidad de placa en `.local/nodes.json`.

En **Conexión al AP y diagnóstico**, selecciona el receptor, introduce el SSID,
la contraseña y la IP del router; pulsa **Guardar en el XIAO y conectar**. Repite
por receptor. La contraseña se guarda en el propio XIAO, no en el repositorio
ni en las capturas. La aplicación no cifra ese almacenamiento NVS.

Configuración del montaje probado:

| Ajuste | Valor |
| --- | --- |
| Banda / ancho / canal | 2.4 GHz / 20 MHz / canal fijo, por ejemplo 6 |
| Modo Wi-Fi / seguridad | g/n mixed / WPA2-Personal AES |
| WMM / AP Isolation / Airtime Fairness | Activado / desactivado / desactivado |
| Potencia de transmisión | High |
| Modo del equipo | Access Point, con DHCP operativo |
| SSID / IP del AP | `CSI_LAB` / `192.168.50.1` |
| Rango DHCP | `192.168.50.100` a `192.168.50.199` |
| Solicitudes de prueba por RX | 100/s, generador `sequential` |

El XIAO solicita tráfico y extrae CSI de las tramas recibidas del AP; el RTT del
ping no es el indicador de movimiento. El AP debe responder a las solicitudes.
La PC puede conservar su conexión habitual a Internet: no necesita conectarse
a CSI_LAB. El router no necesita WAN.

Para elegir los puertos manualmente:

```powershell
python host/capture.py --rx1 COMxx --rx2 COMyy --open
```

Con uno, omite `--rx2`; con tres, agrega `--rx3 COMzz`. Si retiras una placa de
forma definitiva, puedes eliminar su entrada de `.local/nodes.json` para liberar
el alias reservado. No compartas ese archivo como configuración universal.

## 3. Referencia de amplitud y funcionamiento sin PC

Al arrancar y recibir CSI, aparecen resultados provisionales. El receptor espera
20 segundos y obtiene una referencia de amplitud durante 15 segundos de datos
utilizables. BOOT pulsado 1.5 segundos o el botón de la interfaz permiten repetirla.
No mantengas BOOT mientras enciendes la placa: entra en modo de programación.

La referencia normaliza amplitudes; **no aprende ni resta una actividad de fondo**.
Repítela si cambias el montaje. Un corte temporal de datos conserva la referencia;
los cambios de formato que la invalidan se muestran y requieren recuperación.

Una vez configurado, cada RX puede alimentarse con batería o cargador USB:
continúa procesando y el LED representa su score local, relativo al máximo
observado. La interfaz y la fusión entre receptores necesitan los RX conectados
por USB a la PC. El router se alimenta por separado.

## 4. Procesamiento y parámetros

Los valores iniciales están en `firmware/common/experiment_config.h`; los principales se cambian en **Ajustes**.
Los valores guardados en el XIAO prevalecen sobre los iniciales del código.

El detector utiliza las subportadoras útiles de un único LTF, normaliza cada
paquete por su media espacial y calcula dispersión robusta y coherencia temporal.
El banco combina escalas vecinas que respaldan las mismas subportadoras y aplica
confirmación causal. Las escalas no representan distancia ni velocidad del objeto.

| Parámetro | Inicial | Efecto |
| --- | ---: | --- |
| `fast_window` / `window` | 25 / 200 | Extremos del banco: 250 ms a 2 s |
| `scale_count` | 7 | Escalas 250, 350, 500, 710, 1000, 1410 y 2000 ms |
| `consensus_scales` | 2 | Respaldo entre escalas vecinas |
| `confirm_hops` | 2 | Mediana causal de las últimas 3 salidas |
| `hop` | 10 | Salida nominal cada 100 ms |
| `top_k` | 12 | Agregación de contribuciones coherentes |
| `enter_score` / `exit_score` | 4 / 2.5 | Umbrales de actividad con histéresis |
| `detector_kind` | 2 | Banco; 1 conserva el dual y 0 el detector CV anterior |
| `carrier_mode` | 1 | Todas las posiciones útiles |

Una escala necesita al menos 70% de cobertura temporal y una tasa efectiva de
50 Hz. Si faltan suficientes datos, aparece INVALID y un hueco. No se interpolan
los paquetes perdidos ni se convierten en ausencia de movimiento. La tasa RF
real puede variar aunque el hop y los parámetros sean iguales.

## 5. Resultados individuales y conjuntos

La gráfica mantiene RX1 azul, RX2 naranja y RX3 verde. **Ver escalas de** permite
inspeccionar el banco de cada receptor. Los ajustes se envían al RX seleccionado;
los receptores de la fusión deben usar firmware y procesamiento compatibles.

La curva violeta usa un índice normalizado, con umbral de entrada 1:

```text
xi = score_del_receptor / enter_score_del_receptor
conjunto = min(max(xi), 2 * min(xi))
```

La respuesta fuerte necesita respaldo en los demás enlaces. Se alinean features
por sus timestamps y una estimación del desfase USB, con unos 200 ms de espera
adicional y tolerancia inicial de 150 ms. Es aproximado; no sincroniza las radios.
Las constantes están al principio de `host/joint_activity.py`.

La entrada requiere 2 pares por encima de 1; la salida, 3 por debajo de 0.625.
La correlación de los últimos 5 segundos es un diagnóstico separado para dos RX;
requiere al menos 30 pares y variación suficiente. No es una probabilidad ni
una prueba de que la causa sea movimiento. Receptores cercanos pueden mostrar
duraciones y amplitudes distintas por la respuesta de sus enlaces y sus huecos.

**Con tres RX, esta versión todavía exige datos válidos de los tres y utiliza
el mínimo de los tres. La propuesta de confirmar “dos de tres” no está implementada.**
Un enlace débil puede limitar el índice. Con uno, SINGLE_RX indica que no existe
confirmación entre placas. Si falta un RX previsto, el conjunto queda INVALID.

## 6. Capturas y archivos publicados

Cada ejecución crea una carpeta `captures/session_...` con `scores.csv` para
resultados individuales, `joint.csv` para la fusión, diagnósticos y eventos.
Puedes etiquetar reposo y movimiento desde la interfaz. El CSI crudo está apagado
por defecto; se activa al ejecutar el capturador con `--raw`.

La publicación de esta variante contiene fuentes, manifiestos y locks de dependencias,
configuración, interfaz, licencias e instrucciones. Las compilaciones,
componentes descargados, capturas, configuración local, cachés y herramientas
de prueba se conservan fuera de Git mediante `.gitignore`.

El código deriva del proyecto MIT original; consulta [LICENSE](LICENSE).
La adquisición toma como referencia el [ejemplo oficial de Espressif](https://github.com/espressif/esp-csi/tree/master/examples/get-started/csi_recv_router).