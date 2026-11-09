/*
 * servidorISE.c - Control Proceso Principal ISE
 *
 * Uso: ./servidorISE -b <tamB> -p <PipeN> -t <time> -a <agentes>   (flags en cualquier orden)
 *
 * Estructura:
 *   - Proceso padre : crea el pipe nominal, lanza al hijo, espera su fin y
 *                     muestra si termino con exito.
 *   - Proceso hijo  : ejecuta el servidor con 3 tipos de hilos:
 *       * Hilo temporizador : espera <time> segundos y marca el fin.
 *       * Hilo recolector   : lee el pipe y reparte cada registro al buffer
 *                             del agente correspondiente.
 *       * Hilo por agente   : consume su buffer (tamB registros), acumula
 *                             promedios y emite el reporte parcial.
 *   - Sincronizacion productor/consumidor con semaforos POSIX (vacios/llenos).
 *
 * Compilar: gcc -Wall -Wextra -pedantic -std=c11 -pthread -o servidorISE servidorISE.c
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ise.h"

#define MAX_AGENTES  4
#define T_MIN        30
#define T_MAX        120
#define TAMB_MAX     1000000
#define SENT_FIN     (-1)   /* centinela: el agente termino su envio   */
#define SENT_TIMEOUT (-2)   /* centinela: se agoto el tiempo del server */

typedef struct { int sector; double valor; } registro_t;

typedef struct {
    pid_t       pid;
    char        pais[3];
    registro_t *buf;                 /* buffer circular de tamB registros */
    int         tam, in, out;
    sem_t       vacios, llenos;
    pthread_t   hilo;
    int         fin_recibido;
    double      suma[NUM_SECTORES];
    long        cuenta[NUM_SECTORES];
} agente_t;

static const char *const SECTORES[NUM_SECTORES] = {
    "Industria", "Servicios", "Consumidores", "Comercio", "Construccion"
};
static const int PESOS[NUM_SECTORES] = { 40, 30, 20, 5, 5 };

static agente_t agentes[MAX_AGENTES];
static int n_agentes = 0;
static int max_agentes, tam_b, tiempo;
static const char *nombre_pipe;
static int fd_pipe = -1, fd_aux = -1;
static volatile sig_atomic_t terminar = 0;
static pthread_mutex_t mtx_print = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------ Utilidades ------------------------------- */

static void esperar(sem_t *s)
{
    while (sem_wait(s) != 0 && errno == EINTR) { }
}

/* Recomendacion segun el enunciado: >50 Vender, <50 Comprar, =50 Neutro. */
static const char *recomendar(double ise)
{
    long v = (long)(ise * 100.0 + 0.5);   /* comparacion a 2 decimales */
    if (v > 5000) return "VENDER";
    if (v < 5000) return "COMPRAR";
    return "NEUTRO";
}

/* ISE = media ponderada de los promedios por sector (pesos 40/30/20/5/5);
 * si falta algun sector se renormalizan los pesos de los presentes. */
static void imprimir_reporte(const char *titulo, const char *pais,
                             const double suma[], const long cuenta[])
{
    long total = 0;
    double acum = 0.0;
    int pesos = 0;

    pthread_mutex_lock(&mtx_print);
    printf("\n===== %s (%s) =====\n", titulo, pais);
    for (int s = 0; s < NUM_SECTORES; s++) {
        if (cuenta[s] > 0) {
            double prom = suma[s] / (double)cuenta[s];
            printf("  %-13s: %6ld encuestas, promedio %.2f%%\n",
                   SECTORES[s], cuenta[s], prom);
            acum += PESOS[s] * prom;
            pesos += PESOS[s];
            total += cuenta[s];
        } else {
            printf("  %-13s: sin datos\n", SECTORES[s]);
        }
    }
    if (total == 0) {
        printf("  Sin encuestas recibidas.\n");
    } else {
        double ise = acum / pesos;
        printf("  Total: %ld | ISE: %.2f%% -> %s\n", total, ise, recomendar(ise));
    }
    fflush(stdout);
    pthread_mutex_unlock(&mtx_print);
}

/* ------------------------ Buffer por agente (productor) ------------------ */

static void encolar(agente_t *a, int sector, double valor)
{
    esperar(&a->vacios);
    a->buf[a->in].sector = sector;
    a->buf[a->in].valor = valor;
    a->in = (a->in + 1) % a->tam;
    sem_post(&a->llenos);
}

/* ------------------------------- Hilos ----------------------------------- */

static void *hilo_consumidor(void *arg)
{
    agente_t *a = arg;
    registro_t r;

    for (;;) {
        esperar(&a->llenos);
        r = a->buf[a->out];
        a->out = (a->out + 1) % a->tam;
        sem_post(&a->vacios);
        if (r.sector < 0) break;                 /* centinela */
        a->suma[r.sector] += r.valor;
        a->cuenta[r.sector]++;
    }
    if (r.sector == SENT_FIN) {
        imprimir_reporte("REPORTE PARCIAL", a->pais, a->suma, a->cuenta);
    }
    return NULL;
}

static void *hilo_temporizador(void *arg)
{
    unsigned restante = (unsigned)tiempo;
    (void)arg;
    while (restante > 0) restante = sleep(restante);
    terminar = 1;
    return NULL;
}

static agente_t *buscar(pid_t pid)
{
    for (int i = 0; i < n_agentes; i++) {
        if (agentes[i].pid == pid) return &agentes[i];
    }
    return NULL;
}

static void conectar_agente(const mensaje_t *m)
{
    agente_t *a;

    if (buscar(m->pid) != NULL) return;
    if (n_agentes >= max_agentes) {
        printf("Agente %.2s (pid %d) rechazado: maximo de %d agentes\n",
               m->pais, (int)m->pid, max_agentes);
        return;
    }
    a = &agentes[n_agentes];
    memset(a, 0, sizeof *a);
    a->pid = m->pid;
    memcpy(a->pais, m->pais, 2);
    a->tam = tam_b;
    a->buf = calloc((size_t)tam_b, sizeof(registro_t));
    if (a->buf == NULL) { perror("calloc"); return; }
    if (sem_init(&a->vacios, 0, (unsigned)tam_b) != 0) {
        perror("sem_init"); free(a->buf); return;
    }
    if (sem_init(&a->llenos, 0, 0) != 0) {
        perror("sem_init"); sem_destroy(&a->vacios); free(a->buf); return;
    }
    if (pthread_create(&a->hilo, NULL, hilo_consumidor, a) != 0) {
        fprintf(stderr, "Error: no se pudo crear el hilo del agente\n");
        sem_destroy(&a->vacios); sem_destroy(&a->llenos); free(a->buf);
        return;
    }
    n_agentes++;
    printf("Agente conectado: %s (pid %d)\n", a->pais, (int)a->pid);
    fflush(stdout);
}

static void procesar(const mensaje_t *m)
{
    agente_t *a;

    if (m->tipo == MSG_HELLO) { conectar_agente(m); return; }
    a = buscar(m->pid);
    if (a == NULL || a->fin_recibido) return;     /* agente no registrado */

    if (m->tipo == MSG_DATO) {
        if (m->sector >= 0 && m->sector < NUM_SECTORES &&
            m->sentimiento >= 0.0 && m->sentimiento <= 100.0) {
            encolar(a, m->sector, m->sentimiento);
        }
    } else if (m->tipo == MSG_FIN) {
        a->fin_recibido = 1;
        encolar(a, SENT_FIN, 0.0);
    }
}

static void *hilo_recolector(void *arg)
{
    mensaje_t m;
    size_t off = 0;
    (void)arg;

    while (!terminar) {
        struct pollfd p = { fd_pipe, POLLIN, 0 };
        int r = poll(&p, 1, 200);               /* despierta para ver 'terminar' */
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("poll"); break;
        }
        if (r == 0) continue;
        ssize_t n = read(fd_pipe, (char *)&m + off, sizeof m - off);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            perror("read"); break;
        }
        off += (size_t)n;
        if (off < sizeof m) continue;
        off = 0;
        procesar(&m);
    }
    return NULL;
}

/* ------------------------- Proceso hijo (servidor) ----------------------- */

static int ejecutar_servidor(void)
{
    pthread_t t_rec, t_tim;
    int estado = EXIT_SUCCESS;
    double gs[NUM_SECTORES] = { 0 };
    long gc[NUM_SECTORES] = { 0 };

    fd_pipe = open(nombre_pipe, O_RDONLY | O_NONBLOCK);
    if (fd_pipe < 0) { perror("open (lectura del pipe)"); return EXIT_FAILURE; }
    /* Escritor ficticio: evita EOF/POLLHUP cuando no hay agentes conectados. */
    fd_aux = open(nombre_pipe, O_WRONLY);
    if (fd_aux < 0) { perror("open (escritor auxiliar)"); close(fd_pipe); return EXIT_FAILURE; }

    printf("Servidor ISE activo: pipe '%s', %d s, buffer %d, max %d agentes\n",
           nombre_pipe, tiempo, tam_b, max_agentes);
    fflush(stdout);

    if (pthread_create(&t_rec, NULL, hilo_recolector, NULL) != 0) {
        fprintf(stderr, "Error: no se pudo crear el hilo recolector\n");
        close(fd_pipe); close(fd_aux);
        return EXIT_FAILURE;
    }
    if (pthread_create(&t_tim, NULL, hilo_temporizador, NULL) != 0) {
        fprintf(stderr, "Error: no se pudo crear el hilo temporizador\n");
        terminar = 1;
        estado = EXIT_FAILURE;
    } else {
        pthread_join(t_tim, NULL);
    }
    pthread_join(t_rec, NULL);

    /* Cierra los hilos de agentes: los que no terminaron reciben centinela. */
    for (int i = 0; i < n_agentes; i++) {
        if (!agentes[i].fin_recibido) encolar(&agentes[i], SENT_TIMEOUT, 0.0);
        pthread_join(agentes[i].hilo, NULL);
    }
    close(fd_pipe);
    close(fd_aux);

    printf("\n######## RESUMEN GENERAL ISE ########\n");
    if (n_agentes == 0) printf("No se conecto ningun agente.\n");
    for (int i = 0; i < n_agentes; i++) {
        agente_t *a = &agentes[i];
        imprimir_reporte(a->fin_recibido ? "RESUMEN FINAL"
                                         : "RESUMEN FINAL (INCOMPLETO: TIEMPO AGOTADO)",
                         a->pais, a->suma, a->cuenta);
        for (int s = 0; s < NUM_SECTORES; s++) {
            gs[s] += a->suma[s];
            gc[s] += a->cuenta[s];
        }
        sem_destroy(&a->vacios);
        sem_destroy(&a->llenos);
        free(a->buf);
    }
    if (n_agentes > 0) imprimir_reporte("ISE GLOBAL", "TODOS", gs, gc);
    return estado;
}

/* ---------------------------- Argumentos / main -------------------------- */

static void mostrar_uso(void)
{
    fprintf(stderr,
            "Uso: ./servidorISE -b <tamB> -p <PipeN> -t <time> -a <agentes>\n"
            "  -b  tamano del buffer por agente en registros (1..%d)\n"
            "  -p  nombre del pipe nominal\n"
            "  -t  tiempo de servicio en segundos (%d..%d)\n"
            "  -a  maximo de agentes (1..%d)\n",
            TAMB_MAX, T_MIN, T_MAX, MAX_AGENTES);
}

static int leer_entero(const char *texto, long min, long max, char flag, int *salida)
{
    char *fin = NULL;
    long v;

    if (texto == NULL || !isdigit((unsigned char)texto[0])) {
        fprintf(stderr, "Error: -%c requiere un entero positivo\n", flag);
        return -1;
    }
    errno = 0;
    v = strtol(texto, &fin, 10);
    if (errno == ERANGE || *fin != '\0' || v < min || v > max) {
        fprintf(stderr, "Error: valor de -%c invalido (entero entre %ld y %ld)\n",
                flag, min, max);
        return -1;
    }
    *salida = (int)v;
    return 0;
}

static int crear_pipe(const char *nombre)
{
    struct stat st;

    if (mkfifo(nombre, 0666) == 0) return 0;
    if (errno != EEXIST) { perror("mkfifo"); return -1; }
    /* Ya existe: solo se reutiliza si es un FIFO (nunca se borra otro tipo). */
    if (stat(nombre, &st) != 0 || !S_ISFIFO(st.st_mode)) {
        fprintf(stderr, "Error: '%s' existe y no es un pipe nominal\n", nombre);
        return -1;
    }
    if (unlink(nombre) != 0 || mkfifo(nombre, 0666) != 0) {
        perror("mkfifo"); return -1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    const char *a_b = NULL, *a_p = NULL, *a_t = NULL, *a_a = NULL;
    const char **destino;
    int op, estado;
    pid_t hijo;

    opterr = 0;
    while ((op = getopt(argc, argv, "b:p:t:a:")) != -1) {
        switch (op) {
        case 'b': destino = &a_b; break;
        case 'p': destino = &a_p; break;
        case 't': destino = &a_t; break;
        case 'a': destino = &a_a; break;
        default:
            fprintf(stderr, "Error: flag desconocido o sin valor\n");
            mostrar_uso();
            return EXIT_FAILURE;
        }
        if (*destino != NULL) {
            fprintf(stderr, "Error: flag -%c repetido\n", op);
            mostrar_uso();
            return EXIT_FAILURE;
        }
        *destino = optarg;
    }
    if (optind < argc || a_b == NULL || a_p == NULL || a_t == NULL || a_a == NULL) {
        fprintf(stderr, "Error: faltan flags obligatorios o hay argumentos sobrantes\n");
        mostrar_uso();
        return EXIT_FAILURE;
    }
    if (leer_entero(a_b, 1, TAMB_MAX, 'b', &tam_b) != 0 ||
        leer_entero(a_t, T_MIN, T_MAX, 't', &tiempo) != 0 ||
        leer_entero(a_a, 1, MAX_AGENTES, 'a', &max_agentes) != 0) {
        mostrar_uso();
        return EXIT_FAILURE;
    }
    if (a_p[0] == '\0') {
        fprintf(stderr, "Error: el nombre del pipe (-p) esta vacio\n");
        return EXIT_FAILURE;
    }
    nombre_pipe = a_p;

    if (crear_pipe(nombre_pipe) != 0) return EXIT_FAILURE;

    hijo = fork();
    if (hijo < 0) {
        perror("fork");
        unlink(nombre_pipe);
        return EXIT_FAILURE;
    }
    if (hijo == 0) {
        return ejecutar_servidor();
    }

    /* Padre: espera al hijo y reporta exito o fallo. */
    while (waitpid(hijo, &estado, 0) < 0) {
        if (errno != EINTR) { perror("waitpid"); unlink(nombre_pipe); return EXIT_FAILURE; }
    }
    unlink(nombre_pipe);
    if (WIFEXITED(estado) && WEXITSTATUS(estado) == EXIT_SUCCESS) {
        printf("\nServidor ISE finalizo con exito.\n");
        return EXIT_SUCCESS;
    }
    fprintf(stderr, "\nServidor ISE NO finalizo correctamente.\n");
    return EXIT_FAILURE;
}
