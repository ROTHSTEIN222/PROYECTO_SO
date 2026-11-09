/*
 * agentePais.c - Agente de lectura de encuestas por pais
 *
 * Uso: ./agentePais -n <encuestas_XX.csv> -p <pipeNom>   (flags en cualquier orden)
 *
 * Lee el CSV, calcula el total, reserva memoria, se conecta al pipe nominal
 * creado por servidorISE, envia las encuestas mostrando el progreso por lotes
 * de 500 y termina devolviendo el control a la terminal.
 *
 * Compilar: gcc -Wall -Wextra -pedantic -std=c11 -o agentePais agentePais.c
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ise.h"

#define LOTE        500
#define TAM_LINEA   256

typedef struct { int sector; double valor; } registro_t;

static void mostrar_uso(void)
{
    fprintf(stderr,
            "Uso: ./agentePais -n <encuestas_XX.csv> -p <pipeNom>\n"
            "  -n  fichero CSV de encuestas\n"
            "  -p  nombre del pipe nominal creado por servidorISE\n");
}

/* Escribe todo el mensaje (reintenta si lo interrumpe una senal). */
static int escribir_todo(int fd, const void *datos, size_t n)
{
    const char *p = datos;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int enviar(int fd, int tipo, const char *pais, int sector, double valor)
{
    mensaje_t m;
    memset(&m, 0, sizeof m);
    m.tipo = tipo;
    m.pid = getpid();
    memcpy(m.pais, pais, 2);
    m.sector = sector;
    m.sentimiento = valor;
    return escribir_todo(fd, &m, sizeof m);
}

/*
 * cargar_csv: 1a pasada cuenta lineas (para reservar memoria), 2a pasada
 * interpreta cada fila. Las filas invalidas se descartan y se informan.
 * Devuelve el arreglo reservado (o NULL en error); *n = registros validos.
 */
static registro_t *cargar_csv(const char *ruta, long *n, char pais[3])
{
    FILE *f = fopen(ruta, "r");
    char linea[TAM_LINEA];
    long lineas = 0, invalidas = 0;
    registro_t *datos;

    *n = 0;
    pais[0] = '\0';
    if (f == NULL) { perror(ruta); return NULL; }

    if (fgets(linea, sizeof linea, f) == NULL) {      /* encabezado */
        fprintf(stderr, "Error: '%s' esta vacio\n", ruta);
        fclose(f);
        return NULL;
    }
    while (fgets(linea, sizeof linea, f) != NULL) lineas++;
    if (ferror(f) || lineas == 0) {
        fprintf(stderr, "Error: '%s' no contiene encuestas\n", ruta);
        fclose(f);
        return NULL;
    }
    datos = malloc((size_t)lineas * sizeof *datos);
    if (datos == NULL) { perror("malloc"); fclose(f); return NULL; }

    rewind(f);
    if (fgets(linea, sizeof linea, f) == NULL) {      /* saltar encabezado */
        free(datos); fclose(f); return NULL;
    }
    while (fgets(linea, sizeof linea, f) != NULL) {
        char p[3];
        int sector;
        double valor;

        if (linea[0] == '\n' || linea[0] == '\r') continue;     /* linea vacia */
        if (sscanf(linea, "%*d,%*d,%2[A-Za-z],%*[^,],%d,%lf", p, &sector, &valor) != 3 ||
            sector < 0 || sector >= NUM_SECTORES || valor < 0.0 || valor > 100.0 ||
            (pais[0] != '\0' && strcmp(p, pais) != 0)) {
            invalidas++;
            continue;
        }
        if (pais[0] == '\0') { memcpy(pais, p, 2); pais[2] = '\0'; }
        datos[*n].sector = sector;
        datos[*n].valor = valor;
        (*n)++;
    }
    fclose(f);

    if (invalidas > 0) fprintf(stderr, "Aviso: %ld filas invalidas descartadas\n", invalidas);
    if (*n == 0) {
        fprintf(stderr, "Error: '%s' no tiene encuestas validas\n", ruta);
        free(datos);
        return NULL;
    }
    return datos;
}

int main(int argc, char *argv[])
{
    const char *a_n = NULL, *a_p = NULL;
    const char **destino;
    int op, fd, flags;
    long total, enviadas;
    char pais[3];
    registro_t *datos;

    signal(SIGPIPE, SIG_IGN);        /* si el servidor cierra, write() devuelve EPIPE */

    opterr = 0;
    while ((op = getopt(argc, argv, "n:p:")) != -1) {
        switch (op) {
        case 'n': destino = &a_n; break;
        case 'p': destino = &a_p; break;
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
    if (optind < argc || a_n == NULL || a_p == NULL || a_n[0] == '\0' || a_p[0] == '\0') {
        fprintf(stderr, "Error: faltan flags obligatorios, hay valores vacios o argumentos sobrantes\n");
        mostrar_uso();
        return EXIT_FAILURE;
    }

    datos = cargar_csv(a_n, &total, pais);
    if (datos == NULL) return EXIT_FAILURE;

    printf("\n=== THREAD - ENVIO DE ENCUESTAS ===\n\n");
    printf("Pais: %s\nPipe: %s\nLeidas %ld encuestas.\n", pais, a_p, total);
    printf("Conectando al proceso principal...\n");
    fflush(stdout);

    /* O_NONBLOCK: si el servidor no esta activo, falla de inmediato (ENXIO). */
    fd = open(a_p, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("No se pudo conectar al pipe (el servidor ISE debe estar activo)");
        free(datos);
        return EXIT_FAILURE;
    }
    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        perror("fcntl");
        close(fd); free(datos);
        return EXIT_FAILURE;
    }
    printf("Conectado.\n");

    if (enviar(fd, MSG_HELLO, pais, 0, 0.0) != 0) goto fallo_envio;

    printf("Enviando...\n");
    for (enviadas = 0; enviadas < total; enviadas++) {
        if (enviar(fd, MSG_DATO, pais, datos[enviadas].sector, datos[enviadas].valor) != 0) {
            goto fallo_envio;
        }
        if ((enviadas + 1) % LOTE == 0 && enviadas + 1 < total) {
            printf("  %ld/%ld\n", enviadas + 1, total);
            fflush(stdout);
        }
    }
    printf("Enviadas %ld/%ld encuestas.\n", total, total);

    if (enviar(fd, MSG_FIN, pais, 0, 0.0) != 0) goto fallo_envio;
    printf("Mensaje de finalizacion enviado.\n");
    printf("Envio %s finalizado satisfactoriamente!!.\n", pais);

    if (close(fd) != 0) perror("close");
    free(datos);
    return EXIT_SUCCESS;

fallo_envio:
    perror("Error al enviar (el servidor pudo haber cerrado el pipe)");
    close(fd);
    free(datos);
    return EXIT_FAILURE;
}
