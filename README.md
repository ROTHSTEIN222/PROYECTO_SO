# PROYECTO_SO - Indicador de Sentimiento Económico (ISE)

Proyecto de Sistemas Operativos 2026-30. Simula el Indicador de Sentimiento
Económico usando procesos, hilos POSIX, semáforos y pipes nominales en C.

## Componentes

| Programa | Qué hace |
|---|---|
| `generadorCSV` | Crea el fichero `encuestas_XX.csv` de un país (ES, FR o IT) con un porcentaje del máximo de encuestas de la Tabla 1. |
| `servidorISE` | Crea el pipe nominal, recibe las encuestas de los agentes durante un tiempo, muestra un reporte parcial por agente y un resumen general al terminar. |
| `agentePais` | Lee un CSV de encuestas y lo envía al servidor por el pipe nominal, mostrando el progreso. |

Archivos del código:

- `ise.h` / `ise.c`: constantes y funciones comunes (Tabla 1, pesos, formato del mensaje).
- `generadorCSV.c`, `agentePais.c`, `servidorISE.c`: un programa cada uno.
- `Makefile`: compila todo.

## Cómo funciona (resumen)

- El **servidor** crea el pipe nominal y hace `fork()`:
  - El **proceso padre** espera a que el hijo termine y dice si terminó bien o no.
  - El **proceso hijo** es el que atiende a los agentes. Usa hilos:
    - Un **hilo temporizador** que espera `<time>` segundos y avisa que se acabó el tiempo.
    - Un **hilo recolector** que lee el pipe y pone cada encuesta en el buffer del agente que la envió.
    - Un **hilo por agente** que saca las encuestas de su buffer y calcula los promedios.
- Cada agente tiene un buffer de `<tamB>` registros. El recolector produce y el
  hilo del agente consume, sincronizados con semáforos POSIX
  (productor-consumidor visto en clase).
- **ISE** = media ponderada del promedio de sentimiento de cada sector:
  Industria 40 %, Servicios 30 %, Consumidores 20 %, Comercio 5 %, Construcción 5 %.
- **Recomendación**: ISE > 50 % Vender, ISE < 50 % Comprar, ISE = 50 % Neutro.

## Compilar

Pensado para Linux (necesita `gcc` y `make`).

```bash
make          # compila generadorCSV, agentePais y servidorISE
make clean    # borra los .o y los ejecutables
```

## Uso

En todos los programas los flags pueden ir en cualquier orden.

### 1. Generar las encuestas

```bash
./generadorCSV -p <pais> -e <porcentaje>
```

- `-p`: país, `ES`, `FR` o `IT`.
- `-e`: porcentaje de encuestas a generar, entero de 1 a 100.

Ejemplo:

```bash
./generadorCSV -p FR -e 75
```

```
Generando fichero encuestas_FR.csv para país FR con 75% de encuestas
Máximos por sector:
  Industria    : 2888/3850 encuestas (75% del máximo)
  Servicios    : 3180/4240 encuestas (75% del máximo)
  Consumidores : 1253/1670 encuestas (75% del máximo)
  Comercio     : 2145/2860 encuestas (75% del máximo)
  Construcción : 18015/24020 encuestas (75% del máximo)

Total encuestas: 27481
```

### 2. Iniciar el servidor (primero, en su propia terminal)

```bash
./servidorISE -b <tamB> -p <PipeN> -t <time> -a <hilos>
```

- `-b`: tamaño del buffer de cada agente, en registros (1 o más).
- `-p`: nombre del pipe nominal (lo crea el servidor).
- `-t`: tiempo que atiende el servidor, de 30 a 120 segundos.
- `-a`: cantidad máxima de agentes, de 1 a 4 (un hilo por agente).

Ejemplo:

```bash
./servidorISE -b 10 -p ise_fifo -t 60 -a 3
```

### 3. Lanzar los agentes (cada uno en otra terminal)

```bash
./agentePais -n <encuestas_XX.csv> -p <pipeNom>
```

- `-n`: fichero CSV de encuestas.
- `-p`: el mismo pipe que se le dio al servidor.

Ejemplo:

```bash
./agentePais -n encuestas_ES.csv -p ise_fifo
```

```
╔═══════════════════════════════════════════════════════════════╗
║         THREAD - ENVÍO DE ENCUESTAS                           ║
╚═══════════════════════════════════════════════════════════════╝

País: ES
Pipe: ise_fifo
Leídas 5132 encuestas.
Conectando al proceso principal...
Conectado.
Enviando...
  500/5132
  ...
  5000/5132
 Enviadas 5132/5132 encuestas.
Mensaje de finalización enviado.
Envío ES finalizado satisfactoriamente!!.
```

Cuando un agente termina, el servidor imprime su **reporte parcial**. Cuando
se acaba el tiempo, imprime el **resumen general ISE** y el padre indica si el
servidor finalizó con éxito. El pipe nominal se borra al terminar.

## Ejemplo completo

```bash
make
./generadorCSV -p ES -e 78
./generadorCSV -p FR -e 75
./generadorCSV -p IT -e 100

# Terminal 1
./servidorISE -b 10 -p ise_fifo -t 60 -a 3

# Terminal 2, 3 y 4 (antes de que pasen los 60 segundos)
./agentePais -n encuestas_ES.csv -p ise_fifo
./agentePais -p ise_fifo -n encuestas_FR.csv
./agentePais -n encuestas_IT.csv -p ise_fifo
```

## Pruebas que se pueden hacer

| Prueba | Comando / cómo | Resultado esperado |
|---|---|---|
| Flags en otro orden | `./servidorISE -t 30 -a 2 -p ise_fifo -b 5` | Funciona igual. |
| Buffer mínimo | Servidor con `-b 1` y varios agentes | Llegan todas las encuestas. |
| Más agentes que `-a` | Servidor con `-a 1` y dos agentes | El segundo es rechazado: el servidor lo indica y el agente muestra "El servidor ISE rechazó la conexión". |
| Agente sin servidor | `./agentePais -n encuestas_ES.csv -p ise_fifo` sin servidor | Error: el pipe no existe. |
| Agente después del tiempo | Lanzar un agente cuando el servidor ya terminó | Error: el pipe no existe. |
| Tiempo agotado a mitad de envío | Servidor con `-t 30`, lanzar un agente con FR al 100 % cerca del segundo 30 | El servidor lo marca como "incompleto" y el agente muestra error al enviar. |
| Servidor sin agentes | `./servidorISE -b 5 -p ise_fifo -t 30 -a 2` y no lanzar agentes | "No se conectó ningún agente." |
| Ctrl+C en el servidor | Presionar Ctrl+C mientras corre | Termina y borra el pipe (`ls ise_fifo` ya no lo encuentra). |
| Tiempo fuera de rango | `./servidorISE -b 5 -p ise_fifo -t 10 -a 2` | Error: time entre 30 y 120. |
| Demasiados agentes | `./servidorISE -b 5 -p ise_fifo -t 30 -a 5` | Error: hilos entre 1 y 4. |
| País inválido | `./generadorCSV -p XX -e 50` | Error: use ES, FR o IT. |
| Porcentaje inválido | `./generadorCSV -p ES -e 0`, `-e 101` o `-e 7a` | Error: entero de 1 a 100. |
| Flag desconocido o faltante | `./agentePais -n encuestas_ES.csv` | Muestra el uso correcto. |
| Fichero inexistente | `./agentePais -n nada.csv -p ise_fifo` | Error al abrir el fichero. |

Para revisar el CSV generado:

```bash
head -5 encuestas_FR.csv                        # encabezado y primeras filas
wc -l encuestas_FR.csv                          # total de líneas (encuestas + 1)
cut -d, -f4 encuestas_FR.csv | sort | uniq -c   # encuestas por sector
```
