/*
 * agentePais.c - Agente de lectura de encuestas por pais
 *
 * Uso: ./agentePais -n <encuestas_XX.csv> -p <pipeNom>   (flags en cualquier orden)
 *
 * Lee el CSV, cuenta las encuestas, reserva memoria, se conecta al pipe
 * nominal creado por servidorISE y envia las encuestas mostrando el progreso.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "ise.h"

#define LOTE      500    /* cada cuantas encuestas se muestra el progreso */
#define TAM_LINEA 256

typedef struct {
    int    sector;
    double sentimiento;
} encuesta_t;

static void uso(void)
{
    fprintf(stderr, "Uso: ./agentePais -n <encuestas_XX.csv> -p <pipeNom>\n");
    fprintf(stderr, "  -n  fichero CSV de encuestas\n");
    fprintf(stderr, "  -p  pipe nominal creado por servidorISE\n");
}

/* SIGUSR1: el servidor rechazo al agente (ya tiene el maximo de agentes) */
static void manejador_rechazo(int sig)
{
    ssize_t n;
    char texto[] = "\nEl servidor ISE rechazó la conexión: ya tiene el máximo de agentes.\n";

    (void) sig;
    n = write(STDERR_FILENO, texto, strlen(texto));
    (void) n;
    _exit(1);
}

/* Envia un mensaje por el pipe; si falla, libera recursos y termina */
static void enviar(int fd, mensaje_t *msg, encuesta_t *encuestas)
{
    if (write(fd, msg, sizeof(mensaje_t)) != sizeof(mensaje_t)) {
        perror("Error al enviar (el servidor cerró el pipe)");
        close(fd);
        free(encuestas);
        exit(1);
    }
}

int main(int argc, char *argv[])
{
    char *fichero = NULL, *pipe_nom = NULL;
    char linea[TAM_LINEA], pais[3] = "", p[3];
    encuesta_t *encuestas;
    mensaje_t msg;
    struct stat st;
    long total = 0, leidas = 0, invalidas = 0, i;
    int fd, sector, k;
    double valor;
    FILE *fp;

    /* Si el servidor cierra el pipe, write() devuelve error (EPIPE)
     * en lugar de que SIGPIPE termine el proceso sin avisar */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, manejador_rechazo);

    if (argc != 5) {
        uso();
        exit(1);
    }
    for (k = 1; k < argc; k += 2) {
        if (strcmp(argv[k], "-n") == 0)
            fichero = argv[k + 1];
        else if (strcmp(argv[k], "-p") == 0)
            pipe_nom = argv[k + 1];
        else {
            fprintf(stderr, "Error: flag desconocido '%s'\n", argv[k]);
            uso();
            exit(1);
        }
    }
    if (fichero == NULL || pipe_nom == NULL) {
        fprintf(stderr, "Error: faltan los flags -n y/o -p\n");
        uso();
        exit(1);
    }

    /* 1. Contar las encuestas del fichero (sin el encabezado) */
    fp = fopen(fichero, "r");
    if (fp == NULL) {
        perror("Error al abrir el fichero de encuestas");
        exit(1);
    }
    if (fgets(linea, TAM_LINEA, fp) == NULL) {
        fprintf(stderr, "Error: el fichero '%s' esta vacio\n", fichero);
        fclose(fp);
        exit(1);
    }
    while (fgets(linea, TAM_LINEA, fp) != NULL)
        total++;
    if (total == 0) {
        fprintf(stderr, "Error: el fichero '%s' no tiene encuestas\n", fichero);
        fclose(fp);
        exit(1);
    }

    /* 2. Reservar memoria para todas las encuestas */
    encuestas = (encuesta_t *) malloc(total * sizeof(encuesta_t));
    if (encuestas == NULL) {
        perror("Error en malloc");
        fclose(fp);
        exit(1);
    }

    /* 3. Volver al inicio y leer cada linea:
     *    ID_Encuesta,ID_Pais,Pais,Sector,ID_Sector,Sentimiento */
    rewind(fp);
    if (fgets(linea, TAM_LINEA, fp) == NULL) {   /* se salta el encabezado */
        perror("Error al leer el fichero");
        exit(1);
    }
    while (leidas < total && fgets(linea, TAM_LINEA, fp) != NULL) {
        /* %*d y %*[^,] leen el campo pero no lo guardan */
        if (sscanf(linea, "%*d,%*d,%2[^,],%*[^,],%d,%lf", p, &sector, &valor) != 3
            || sector < 0 || sector >= NUM_SECTORES
            || valor < 0.0 || valor > 100.0) {
            invalidas++;
            continue;
        }
        if (pais[0] == '\0')
            strcpy(pais, p);
        encuestas[leidas].sector = sector;
        encuestas[leidas].sentimiento = valor;
        leidas++;
    }
    fclose(fp);
    if (invalidas > 0)
        fprintf(stderr, "Aviso: %ld lineas invalidas descartadas\n", invalidas);
    if (leidas == 0) {
        fprintf(stderr, "Error: el fichero '%s' no tiene encuestas validas\n", fichero);
        free(encuestas);
        exit(1);
    }
    total = leidas;

    printf("\n╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║         THREAD - ENVÍO DE ENCUESTAS                           ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
    printf("País: %s\n", pais);
    printf("Pipe: %s\n", pipe_nom);
    printf("Leídas %ld encuestas.\n", total);
    printf("Conectando al proceso principal...\n");
    fflush(stdout);

    /* 4. Conectarse al pipe nominal (debe existir: lo crea el servidor) */
    if (stat(pipe_nom, &st) < 0 || !S_ISFIFO(st.st_mode)) {
        fprintf(stderr, "Error: '%s' no es un pipe nominal activo "
                        "(¿está corriendo servidorISE?)\n", pipe_nom);
        free(encuestas);
        exit(1);
    }
    fd = open(pipe_nom, O_WRONLY);   /* se bloquea hasta que el servidor lo abra */
    if (fd < 0) {
        perror("Error al abrir el pipe");
        free(encuestas);
        exit(1);
    }
    printf("Conectado.\n");

    /* Mensaje inicial: el servidor registra al agente con su nombre */
    memset(&msg, 0, sizeof(msg));
    msg.pid = getpid();
    strcpy(msg.pais, pais);
    strncpy(msg.nombre, fichero, TAM_NOMBRE - 1);
    msg.tipo = MSG_CONEXION;
    enviar(fd, &msg, encuestas);

    /* 5. Enviar las encuestas una por una */
    printf("Enviando...\n");
    msg.tipo = MSG_ENCUESTA;
    for (i = 0; i < total; i++) {
        msg.sector = encuestas[i].sector;
        msg.sentimiento = encuestas[i].sentimiento;
        enviar(fd, &msg, encuestas);
        if ((i + 1) % LOTE == 0 && i + 1 < total) {
            printf("  %ld/%ld\n", i + 1, total);
            fflush(stdout);
        }
    }
    printf(" Enviadas %ld/%ld encuestas.\n", total, total);

    /* 6. Avisar al servidor que este agente termino */
    msg.tipo = MSG_FIN;
    enviar(fd, &msg, encuestas);
    printf("Mensaje de finalización enviado.\n");
    printf("Envío %s finalizado satisfactoriamente!!.\n\n", pais);

    close(fd);
    free(encuestas);
    return 0;
}
