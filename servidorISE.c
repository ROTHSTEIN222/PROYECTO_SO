/*
 * servidorISE.c - Control Proceso Principal ISE
 *
 * Uso: ./servidorISE -b <tamB> -p <PipeN> -t <time> -a <hilos>   (flags en cualquier orden)
 *
 * Estructura (Figura 1 del enunciado):
 *   - Proceso padre: crea el pipe nominal, crea al hijo, espera a que
 *     termine y muestra si finalizo con exito o no.
 *   - Proceso hijo: recibe las encuestas y muestra el resumen final.
 *     Usa estos hilos POSIX:
 *       * Hilo temporizador: espera <time> segundos y avisa el fin.
 *       * Hilo recolector: lee el pipe y deja cada encuesta en el
 *         buffer del agente que la envio (productor).
 *       * Un hilo por agente: saca las encuestas de su buffer, acumula
 *         promedios y emite el reporte parcial (consumidor).
 *   - Cada buffer es circular de <tamB> registros y se sincroniza con
 *     3 semaforos POSIX (huecos, elementos y mutex), como en clase.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "ise.h"

#define MAX_AGENTES  4
#define TIEMPO_MIN   30
#define TIEMPO_MAX   120

/* Valores especiales de sector para avisar al hilo del agente que termine */
#define FIN_AGENTE   -1   /* el agente envio todas sus encuestas */
#define FIN_TIEMPO   -2   /* se acabo el tiempo del servidor     */
#define FIN_CAIDA    -3   /* el agente se cerro antes de terminar */

typedef struct {
    int    sector;
    double sentimiento;
} registro_t;

typedef struct {
    pid_t       pid;
    char        pais[3];
    char        nombre[TAM_NOMBRE];
    registro_t *buffer;          /* buffer circular de tamB registros */
    int         entrada, salida; /* posicion del productor y del consumidor */
    sem_t       huecos;          /* espacios libres del buffer  */
    sem_t       elementos;       /* registros listos para leer  */
    sem_t       mutex;           /* acceso exclusivo al buffer  */
    pthread_t   hilo;
    int         terminado;       /* 1 si ya no va a enviar mas  */
    int         desconectado;    /* 1 si se cerro sin enviar MSG_FIN */
    long        cantidad[NUM_SECTORES];
    double      suma[NUM_SECTORES];
} agente_t;

/* Datos globales: los comparten todos los hilos del proceso */
static agente_t agentes[MAX_AGENTES];
static int num_agentes = 0;
static int tam_buffer, tiempo, max_agentes;
static char *nombre_pipe;
static pid_t pid_hijo = 0;
static pthread_mutex_t mutex_pantalla = PTHREAD_MUTEX_INITIALIZER;

static void uso(void)
{
    fprintf(stderr, "Uso: ./servidorISE -b <tamB> -p <PipeN> -t <time> -a <hilos>\n");
    fprintf(stderr, "  -b  tamano del buffer por agente (registros, >= 1)\n");
    fprintf(stderr, "  -p  nombre del pipe nominal\n");
    fprintf(stderr, "  -t  tiempo de servicio en segundos (%d a %d)\n", TIEMPO_MIN, TIEMPO_MAX);
    fprintf(stderr, "  -a  cantidad maxima de agentes/hilos (1 a %d)\n", MAX_AGENTES);
}

/* Ctrl+C, kill o cierre de la terminal: se termina tambien al hijo y se
 * borra el pipe nominal para no dejar basura en el disco */
static void manejador_salida(int sig)
{
    ssize_t n;

    (void) sig;
    if (pid_hijo > 0)
        kill(pid_hijo, SIGTERM);
    unlink(nombre_pipe);
    n = write(STDOUT_FILENO, "\nServidor ISE interrumpido.\n", 28);
    (void) n;
    _exit(1);
}

/* ---------------------------- Calculos ---------------------------------- */

/* printf cuenta bytes y no letras: en UTF-8 la "ó" ocupa 2 bytes, asi que
 * se suma 1 al ancho por cada byte extra para que las columnas cuadren */
static int ancho(const char *texto, int columnas)
{
    int i;

    for (i = 0; texto[i] != '\0'; i++) {
        if ((texto[i] & 0xC0) == 0x80)   /* byte de continuacion UTF-8 */
            columnas++;
    }
    return columnas;
}

/*
 * ISE = media ponderada de los promedios por sector (pesos 40/30/20/5/5).
 * Si un sector no tiene encuestas, su peso se reparte entre los demas.
 * Devuelve -1 si no hay ninguna encuesta.
 */
static double calcular_ise(double suma[], long cantidad[])
{
    double ise = 0.0;
    int s, peso_total = 0;

    for (s = 0; s < NUM_SECTORES; s++) {
        if (cantidad[s] > 0) {
            ise += PESOS[s] * (suma[s] / cantidad[s]);
            peso_total += PESOS[s];
        }
    }
    if (peso_total == 0)
        return -1.0;
    return ise / peso_total;
}

static void imprimir_reporte_parcial(agente_t *a)
{
    long total = 0;
    double ise;
    int s;

    pthread_mutex_lock(&mutex_pantalla);   /* que no se mezcle con otros hilos */
    printf("\n----- Reporte parcial: %s (%s) -----\n", a->pais, a->nombre);
    if (a->desconectado)
        printf("  El agente se desconectó antes de terminar (datos incompletos)\n");
    for (s = 0; s < NUM_SECTORES; s++) {
        if (a->cantidad[s] > 0)
            printf("  %-*s %6ld encuestas, promedio %6.2f%%\n",
                   ancho(SECTORES[s], 14), SECTORES[s],
                   a->cantidad[s], a->suma[s] / a->cantidad[s]);
        else
            printf("  %-*s %6d encuestas\n", ancho(SECTORES[s], 14), SECTORES[s], 0);
        total += a->cantidad[s];
    }
    ise = calcular_ise(a->suma, a->cantidad);
    printf("  Total recibidas: %ld\n", total);
    printf("  ISE %s: %.2f%% -> %s\n", a->pais, ise, recomendacion(ise));
    fflush(stdout);
    pthread_mutex_unlock(&mutex_pantalla);
}

static void imprimir_resumen_general(void)
{
    double suma[NUM_SECTORES] = { 0 };
    long cantidad[NUM_SECTORES] = { 0 };
    long total;
    double ise;
    int i, s;

    printf("\n╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║                  RESUMEN GENERAL ISE                          ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n");
    if (num_agentes == 0) {
        printf("No se conectó ningún agente.\n");
        return;
    }
    for (i = 0; i < num_agentes; i++) {
        agente_t *a = &agentes[i];
        total = 0;
        for (s = 0; s < NUM_SECTORES; s++) {
            total += a->cantidad[s];
            suma[s] += a->suma[s];          /* acumulado de todos los paises */
            cantidad[s] += a->cantidad[s];
        }
        ise = calcular_ise(a->suma, a->cantidad);
        if (ise < 0)
            printf("  %s (%s): sin encuestas\n", a->pais, a->nombre);
        else
            printf("  %s (%s): %ld encuestas, ISE %.2f%% -> %s%s\n", a->pais,
                   a->nombre, total, ise, recomendacion(ise),
                   a->desconectado ? " (incompleto: agente desconectado)" :
                   !a->terminado ? " (incompleto: tiempo agotado)" : "");
    }

    printf("\n  Promedio por sector (todos los países):\n");
    for (s = 0; s < NUM_SECTORES; s++) {
        if (cantidad[s] > 0)
            printf("  %-*s %6ld encuestas, promedio %6.2f%% (peso %d%%)\n",
                   ancho(SECTORES[s], 14), SECTORES[s], cantidad[s],
                   suma[s] / cantidad[s], PESOS[s]);
        else
            printf("  %-*s %6d encuestas (peso %d%%)\n",
                   ancho(SECTORES[s], 14), SECTORES[s], 0, PESOS[s]);
    }
    ise = calcular_ise(suma, cantidad);
    if (ise < 0)
        printf("\n  ISE global: sin encuestas\n");
    else
        printf("\n  ISE global: %.2f%% -> Recomendación: %s\n", ise, recomendacion(ise));
}

/* ---------------------- Buffer productor-consumidor ---------------------- */

/* Productor (hilo recolector): deja un registro en el buffer del agente */
static void depositar(agente_t *a, int sector, double sentimiento)
{
    sem_wait(&a->huecos);      /* espera a que haya espacio */
    sem_wait(&a->mutex);
    a->buffer[a->entrada].sector = sector;
    a->buffer[a->entrada].sentimiento = sentimiento;
    a->entrada = (a->entrada + 1) % tam_buffer;
    sem_post(&a->mutex);
    sem_post(&a->elementos);   /* avisa que hay un registro nuevo */
}

/* Consumidor: un hilo por agente que vacia su buffer */
static void *hilo_agente(void *arg)
{
    agente_t *a = (agente_t *) arg;
    registro_t r;

    do {
        sem_wait(&a->elementos);   /* espera a que haya registros */
        sem_wait(&a->mutex);
        r = a->buffer[a->salida];
        a->salida = (a->salida + 1) % tam_buffer;
        sem_post(&a->mutex);
        sem_post(&a->huecos);      /* libera un espacio */

        if (r.sector >= 0) {
            a->suma[r.sector] += r.sentimiento;
            a->cantidad[r.sector]++;
        }
    } while (r.sector >= 0);

    /* Reporte parcial si el agente termino o se cayo (no por tiempo) */
    if (r.sector == FIN_AGENTE || r.sector == FIN_CAIDA)
        imprimir_reporte_parcial(a);
    pthread_exit(NULL);
}

/* ----------------------------- Agentes ----------------------------------- */

static agente_t *buscar_agente(pid_t pid)
{
    int i;

    for (i = 0; i < num_agentes; i++) {
        if (agentes[i].pid == pid)
            return &agentes[i];
    }
    return NULL;
}

/* Registra un agente nuevo: buffer, semaforos y su hilo */
static void registrar_agente(mensaje_t *msg)
{
    agente_t *a;
    int error;

    if (num_agentes == max_agentes) {
        pthread_mutex_lock(&mutex_pantalla);
        printf("\nAgente %s (%s) rechazado: ya hay %d agentes conectados.\n",
               msg->pais, msg->nombre, max_agentes);
        pthread_mutex_unlock(&mutex_pantalla);
        /* El pipe va en un solo sentido: se le avisa al agente con una
         * senal. Si el agente ya no existe, kill falla y no pasa nada */
        kill(msg->pid, SIGUSR1);
        return;
    }

    a = &agentes[num_agentes];
    memset(a, 0, sizeof(agente_t));
    a->pid = msg->pid;
    strcpy(a->pais, msg->pais);
    strcpy(a->nombre, msg->nombre);
    a->buffer = (registro_t *) malloc(tam_buffer * sizeof(registro_t));
    if (a->buffer == NULL) {
        perror("Error en malloc");
        exit(1);
    }
    /* El segundo argumento en 0 indica semaforo compartido entre hilos */
    if (sem_init(&a->huecos, 0, tam_buffer) < 0 ||
        sem_init(&a->elementos, 0, 0) < 0 ||
        sem_init(&a->mutex, 0, 1) < 0) {
        perror("Error en sem_init");
        exit(1);
    }
    error = pthread_create(&a->hilo, NULL, hilo_agente, a);
    if (error != 0) {
        fprintf(stderr, "Error en pthread_create: %s\n", strerror(error));
        exit(1);
    }
    num_agentes++;

    pthread_mutex_lock(&mutex_pantalla);
    printf("\nAgente conectado: %s (país %s, pid %d)\n", a->nombre, a->pais, (int) a->pid);
    fflush(stdout);
    pthread_mutex_unlock(&mutex_pantalla);
}

/* Procesa un mensaje del pipe. Devuelve 0 cuando hay que dejar de leer */
static int procesar_mensaje(mensaje_t *msg)
{
    agente_t *a;

    switch (msg->tipo) {
    case MSG_CONEXION:
        registrar_agente(msg);
        break;
    case MSG_ENCUESTA:
        a = buscar_agente(msg->pid);
        if (a != NULL && !a->terminado)   /* si fue rechazado se ignora */
            depositar(a, msg->sector, msg->sentimiento);
        break;
    case MSG_FIN:
        a = buscar_agente(msg->pid);
        if (a != NULL && !a->terminado) {
            a->terminado = 1;
            depositar(a, FIN_AGENTE, 0.0);
        }
        break;
    case MSG_TIEMPO:
        return 0;
    }
    return 1;
}

/*
 * read devolvio 0: ya no queda ningun agente con el pipe abierto. Un agente
 * mantiene el pipe abierto hasta despues de enviar MSG_FIN, asi que si alguno
 * no lo envio es porque se cerro antes (Ctrl+C, kill, error...)
 */
static void revisar_desconectados(void)
{
    int i;

    for (i = 0; i < num_agentes; i++) {
        if (!agentes[i].terminado) {
            agentes[i].terminado = 1;
            agentes[i].desconectado = 1;
            depositar(&agentes[i], FIN_CAIDA, 0.0);
        }
    }
}

/* ------------------------------- Hilos ----------------------------------- */

/* Lee el pipe nominal y reparte cada encuesta al buffer de su agente */
static void *hilo_recolector(void *arg)
{
    mensaje_t msg;
    int fd, n = 0, seguir = 1;

    (void) arg;
    while (seguir) {
        /* open se bloquea hasta que haya un escritor (agente o temporizador) */
        fd = open(nombre_pipe, O_RDONLY);
        if (fd < 0) {
            perror("Error al abrir el pipe");
            exit(1);
        }
        /* Cada write del agente es atomico (< PIPE_BUF), asi que read
         * devuelve mensajes completos. read devuelve 0 cuando ya no queda
         * ningun escritor: se cierra y se vuelve a abrir para esperar otro */
        while (seguir && (n = read(fd, &msg, sizeof(mensaje_t))) > 0) {
            if (n == sizeof(mensaje_t))
                seguir = procesar_mensaje(&msg);
        }
        if (n == 0)
            revisar_desconectados();
        else if (n < 0)
            perror("Error al leer el pipe");
        close(fd);
    }
    pthread_exit(NULL);
}

/* Espera el tiempo del servidor y avisa al recolector por el mismo pipe */
static void *hilo_temporizador(void *arg)
{
    mensaje_t msg;
    int fd;

    (void) arg;
    sleep(tiempo);

    pthread_mutex_lock(&mutex_pantalla);
    printf("\nTiempo agotado (%d segundos).\n", tiempo);
    fflush(stdout);
    pthread_mutex_unlock(&mutex_pantalla);

    /* El recolector puede estar bloqueado en open() o read(): este
     * mensaje lo despierta y le indica que deje de leer */
    fd = open(nombre_pipe, O_WRONLY);
    if (fd < 0) {
        perror("Error al abrir el pipe (temporizador)");
        exit(1);
    }
    memset(&msg, 0, sizeof(msg));
    msg.tipo = MSG_TIEMPO;
    if (write(fd, &msg, sizeof(msg)) != sizeof(msg))
        perror("Error al escribir en el pipe (temporizador)");
    close(fd);
    pthread_exit(NULL);
}

/* ------------------------- Proceso hijo (servidor) ----------------------- */

static int servidor(void)
{
    pthread_t recolector, temporizador;
    int i;

    printf("\n╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║         SERVIDOR ISE - CONTROL PROCESO PRINCIPAL              ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
    printf("Pipe: %s\n", nombre_pipe);
    printf("Tiempo: %d segundos\n", tiempo);
    printf("Buffer: %d registros por agente\n", tam_buffer);
    printf("Agentes máximos: %d\n", max_agentes);
    printf("Esperando agentes...\n");
    fflush(stdout);

    if (pthread_create(&temporizador, NULL, hilo_temporizador, NULL) != 0 ||
        pthread_create(&recolector, NULL, hilo_recolector, NULL) != 0) {
        fprintf(stderr, "Error al crear los hilos del servidor\n");
        return 1;
    }
    pthread_join(temporizador, NULL);
    pthread_join(recolector, NULL);

    /* A los agentes que no terminaron se les avisa que se acabo el tiempo */
    for (i = 0; i < num_agentes; i++) {
        if (!agentes[i].terminado)
            depositar(&agentes[i], FIN_TIEMPO, 0.0);
        pthread_join(agentes[i].hilo, NULL);
    }

    imprimir_resumen_general();

    for (i = 0; i < num_agentes; i++) {
        sem_destroy(&agentes[i].huecos);
        sem_destroy(&agentes[i].elementos);
        sem_destroy(&agentes[i].mutex);
        free(agentes[i].buffer);
    }
    fflush(stdout);
    return 0;
}

/* ------------------------------ main (padre) ----------------------------- */

int main(int argc, char *argv[])
{
    char *arg_b = NULL, *arg_p = NULL, *arg_t = NULL, *arg_a = NULL;
    struct stat st;
    pid_t pid;
    int i, estado;

    /* Se esperan 4 flags con su valor: el orden puede cambiar */
    if (argc != 9) {
        uso();
        exit(1);
    }
    for (i = 1; i < argc; i += 2) {
        if (strcmp(argv[i], "-b") == 0)
            arg_b = argv[i + 1];
        else if (strcmp(argv[i], "-p") == 0)
            arg_p = argv[i + 1];
        else if (strcmp(argv[i], "-t") == 0)
            arg_t = argv[i + 1];
        else if (strcmp(argv[i], "-a") == 0)
            arg_a = argv[i + 1];
        else {
            fprintf(stderr, "Error: flag desconocido '%s'\n", argv[i]);
            uso();
            exit(1);
        }
    }
    if (arg_b == NULL || arg_p == NULL || arg_t == NULL || arg_a == NULL) {
        fprintf(stderr, "Error: faltan flags (-b, -p, -t y -a son obligatorios)\n");
        uso();
        exit(1);
    }
    if (!es_numero(arg_b) || atoi(arg_b) < 1) {
        fprintf(stderr, "Error: tamB '%s' invalido (entero >= 1)\n", arg_b);
        exit(1);
    }
    if (!es_numero(arg_t) || atoi(arg_t) < TIEMPO_MIN || atoi(arg_t) > TIEMPO_MAX) {
        fprintf(stderr, "Error: time '%s' invalido (entre %d y %d segundos)\n",
                arg_t, TIEMPO_MIN, TIEMPO_MAX);
        exit(1);
    }
    if (!es_numero(arg_a) || atoi(arg_a) < 1 || atoi(arg_a) > MAX_AGENTES) {
        fprintf(stderr, "Error: hilos '%s' invalido (entre 1 y %d agentes)\n",
                arg_a, MAX_AGENTES);
        exit(1);
    }
    tam_buffer = atoi(arg_b);
    tiempo = atoi(arg_t);
    max_agentes = atoi(arg_a);
    nombre_pipe = arg_p;

    /* Crear el pipe nominal. Si ya existe, solo se usa si es un FIFO */
    if (mkfifo(nombre_pipe, S_IRUSR | S_IWUSR) < 0) {
        if (errno != EEXIST) {
            perror("Error en mkfifo");
            exit(1);
        }
        if (stat(nombre_pipe, &st) < 0 || !S_ISFIFO(st.st_mode)) {
            fprintf(stderr, "Error: '%s' ya existe y no es un pipe nominal\n", nombre_pipe);
            exit(1);
        }
    }
    signal(SIGINT, manejador_salida);
    signal(SIGTERM, manejador_salida);
    signal(SIGHUP, manejador_salida);
    /* Si se escribe en el pipe sin lector, write devuelve error en vez de
     * que SIGPIPE termine el proceso */
    signal(SIGPIPE, SIG_IGN);

    fflush(stdout);   /* evita que el hijo herede texto pendiente del padre */
    pid = fork();
    if (pid < 0) {
        perror("Error en fork");
        unlink(nombre_pipe);
        exit(1);
    }
    if (pid == 0) {   /* hijo: recibe encuestas y muestra el resumen final */
        signal(SIGINT, SIG_DFL);   /* el padre se encarga de limpiar */
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        exit(servidor());
    }
    pid_hijo = pid;

    /* Padre: espera a que el hijo termine y muestra el resultado */
    if (wait(&estado) < 0)
        perror("Error en wait");
    unlink(nombre_pipe);
    if (WIFEXITED(estado) && WEXITSTATUS(estado) == 0) {
        printf("\nServidor ISE finalizó con éxito.\n");
        return 0;
    }
    if (WIFSIGNALED(estado))
        printf("\nServidor ISE NO finalizó correctamente "
               "(el proceso hijo terminó por la señal %d).\n", WTERMSIG(estado));
    else
        printf("\nServidor ISE NO finalizó correctamente.\n");
    return 1;
}
