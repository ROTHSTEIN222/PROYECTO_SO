/*
 * generadorCSV.c
 * ---------------------------------------------------------------------------
 * Proyecto Sistemas Operativos 2026-30 - Indicador de Sentimiento Económico
 *
 * Genera el fichero encuestas_XX.csv para un país (ES, FR o IT) con un
 * porcentaje de la cantidad máxima de encuestas definida en la Tabla 1.
 *
 * Uso:
 *     ./generadorCSV -p <pais> -e <porcentaje>
 *     ./generadorCSV -e <porcentaje> -p <pais>      (el orden no importa)
 *
 * Ejemplo:
 *     ./generadorCSV -p FR -e 75
 *
 * Compilación:
 *     gcc -Wall -Wextra -pedantic -std=c11 -o generadorCSV generadorCSV.c
 *
 * Manejo de errores:
 *   - Los errores de llamadas al sistema (fopen, fprintf, fclose...) se
 *     reportan con perror(), que usa el valor real de errno.
 *   - Los errores de argumentos (valores inesperados) NO generan errno por sí
 *     solos, así que se fuerza un errno apropiado (EINVAL, ERANGE) antes de
 *     llamar a perror(). Así TODOS los mensajes salen por perror().
 *   - Todos los mensajes de error van a stderr y el programa termina con
 *     EXIT_FAILURE. Si algo falla a mitad de la escritura, el fichero
 *     incompleto se elimina para no dejar basura.
 * ---------------------------------------------------------------------------
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* ----------------------------- Constantes -------------------------------- */

#define NOMBRE_PROG   "generadorCSV"
#define NUM_PAISES    3
#define NUM_SECTORES  5
#define PORC_MIN      1L     /* Un 0 % generaría un fichero vacío: se rechaza */
#define PORC_MAX      100L
#define TAM_MENSAJE   256
#define TAM_NOMBRE    32

/* Siglas en el orden que define ID_Pais (ES=0, FR=1, IT=2). */
static const char *const SIGLAS[NUM_PAISES] = { "ES", "FR", "IT" };

/* Nombres de sector en el orden que define ID_Sector (0..4). */
static const char *const SECTOR_CSV[NUM_SECTORES] = {
    "Industria", "Servicios", "Consumidores", "Comercio", "Construccion"
};
static const char *const SECTOR_CONSOLA[NUM_SECTORES] = {
    "Industria", "Servicios", "Consumidores", "Comercio", "Construcción"
};

/* Tabla 1: máximo de encuestas por país (fila) y sector (columna). */
static const long MAXIMOS[NUM_PAISES][NUM_SECTORES] = {
    { 1760, 1430, 2020, 1080,   290 },   /* ES */
    { 3850, 4240, 1670, 2860, 24020 },   /* FR */
    { 5440, 3100, 2000, 1560,  1000 }    /* IT */
};

/* ------------------------- Reporte de errores ---------------------------- */

/*
 * reportar_error: imprime un mensaje con perror().
 *   codigo_errno != 0 -> se fuerza ese errno (errores de argumentos).
 *   codigo_errno == 0 -> se conserva el errno actual (fallos del sistema).
 * El mensaje admite formato tipo printf.
 */
static void reportar_error(int codigo_errno, const char *formato, ...)
{
    char mensaje[TAM_MENSAJE];
    int errno_guardado = errno;
    va_list args;

    va_start(args, formato);
    vsnprintf(mensaje, sizeof(mensaje), formato, args);
    va_end(args);

    errno = (codigo_errno != 0) ? codigo_errno : errno_guardado;
    perror(mensaje);
}

/* Muestra cómo se usa el programa (siempre por stderr). */
static void mostrar_uso(void)
{
    fprintf(stderr,
            "Uso: ./%s -p <pais> -e <porcentaje>\n"
            "  -p  sigla del país: ES, FR o IT\n"
            "  -e  porcentaje de encuestas a generar (entero entre %ld y %ld)\n"
            "Ejemplo: ./%s -p FR -e 75\n",
            NOMBRE_PROG, PORC_MIN, PORC_MAX, NOMBRE_PROG);
}

/* ------------------------- Validación de entradas ------------------------ */

/*
 * buscar_pais: devuelve el índice (0..NUM_PAISES-1) de la sigla recibida, o
 * -1 si no es válida. Acepta mayúsculas o minúsculas.
 */
static int buscar_pais(const char *texto)
{
    if (texto == NULL || strlen(texto) != 2) {
        return -1;
    }
    for (int i = 0; i < NUM_PAISES; i++) {
        if (strcasecmp(texto, SIGLAS[i]) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * parsear_porcentaje: convierte el texto a entero y valida que sea 1..100.
 * Rechaza vacío, signos, espacios, decimales, letras y desbordamientos.
 * Devuelve 0 si es válido (resultado en *salida), -1 en caso contrario.
 */
static int parsear_porcentaje(const char *texto, long *salida)
{
    char *fin = NULL;
    long valor;

    if (texto == NULL || texto[0] == '\0') {
        reportar_error(EINVAL, NOMBRE_PROG ": el porcentaje (-e) está vacío");
        return -1;
    }
    /* strtol acepta espacios y signos al inicio; aquí solo se permiten dígitos. */
    if (!isdigit((unsigned char)texto[0])) {
        reportar_error(EINVAL,
                       NOMBRE_PROG ": porcentaje '%s' inválido (debe ser un "
                       "número entero positivo)", texto);
        return -1;
    }

    errno = 0;
    valor = strtol(texto, &fin, 10);
    if (errno == ERANGE) {
        reportar_error(ERANGE,
                       NOMBRE_PROG ": porcentaje '%s' demasiado grande", texto);
        return -1;
    }
    if (*fin != '\0') {
        reportar_error(EINVAL,
                       NOMBRE_PROG ": porcentaje '%s' contiene caracteres no "
                       "numéricos", texto);
        return -1;
    }
    if (valor < PORC_MIN || valor > PORC_MAX) {
        reportar_error(ERANGE,
                       NOMBRE_PROG ": porcentaje %ld fuera de rango (debe estar "
                       "entre %ld y %ld)", valor, PORC_MIN, PORC_MAX);
        return -1;
    }

    *salida = valor;
    return 0;
}

/* ------------------------------ Cálculos --------------------------------- */

/*
 * calcular_cantidad: porcentaje del máximo, redondeado al entero más cercano
 * (los .5 suben). Se usa aritmética entera para evitar errores de punto
 * flotante: (max * p + 50) / 100.
 * Ejemplo: FR Industria 75 % -> (3850*75 + 50)/100 = 2888.
 */
static long calcular_cantidad(long maximo, long porcentaje)
{
    return (maximo * porcentaje + 50) / 100;
}

/*
 * sentimiento_aleatorio: devuelve un entero 0..10000 que representa un valor
 * entre 0.00 y 100.00. Se trabaja en enteros para imprimir con punto decimal
 * sin depender del locale del sistema (que podría usar coma).
 */
static int sentimiento_aleatorio(void)
{
    return (int)(rand() / ((double)RAND_MAX + 1.0) * 10001.0);
}

/* ---------------------------- Generación CSV ----------------------------- */

/*
 * generar_csv: crea el fichero y escribe encabezado + una fila por encuesta.
 * Los ID_Encuesta son consecutivos y NO se reinician entre sectores.
 * Devuelve 0 si todo salió bien, -1 si hubo cualquier fallo (en cuyo caso el
 * fichero incompleto se elimina).
 */
static int generar_csv(const char *nombre, int id_pais,
                       const long cantidades[NUM_SECTORES])
{
    FILE *archivo = fopen(nombre, "w");
    long id_encuesta = 0;

    if (archivo == NULL) {
        reportar_error(0, NOMBRE_PROG ": no se pudo crear '%s'", nombre);
        return -1;
    }

    if (fprintf(archivo,
                "ID_Encuesta,ID_Pais,Pais,Sector,ID_Sector,Sentimiento\n") < 0) {
        goto fallo_escritura;
    }

    for (int sector = 0; sector < NUM_SECTORES; sector++) {
        for (long i = 0; i < cantidades[sector]; i++) {
            int valor = sentimiento_aleatorio();
            id_encuesta++;
            if (fprintf(archivo, "%ld,%d,%s,%s,%d,%d.%02d\n",
                        id_encuesta, id_pais, SIGLAS[id_pais],
                        SECTOR_CSV[sector], sector,
                        valor / 100, valor % 100) < 0) {
                goto fallo_escritura;
            }
        }
    }

    /* fclose también puede fallar (p. ej. disco lleno al vaciar el buffer). */
    if (fclose(archivo) != 0) {
        reportar_error(0, NOMBRE_PROG ": error al cerrar '%s'", nombre);
        if (remove(nombre) != 0) {
            reportar_error(0, NOMBRE_PROG ": no se pudo eliminar '%s'", nombre);
        }
        return -1;
    }
    return 0;

fallo_escritura:
    reportar_error(0, NOMBRE_PROG ": error al escribir en '%s'", nombre);
    fclose(archivo);              /* intento de limpieza; el error ya se reportó */
    if (remove(nombre) != 0) {
        reportar_error(0, NOMBRE_PROG ": no se pudo eliminar '%s'", nombre);
    }
    return -1;
}

/* --------------------------------- main ---------------------------------- */

int main(int argc, char *argv[])
{
    const char *arg_pais = NULL;
    const char *arg_porc = NULL;
    int opcion;
    int id_pais;
    long porcentaje = 0;
    long cantidades[NUM_SECTORES];
    long total = 0;
    char nombre[TAM_NOMBRE];
    int escritos;

    /* 1. Lectura de flags en cualquier orden (getopt). opterr = 0 desactiva
     *    los mensajes propios de getopt para dar los nuestros con perror(). */
    opterr = 0;
    while ((opcion = getopt(argc, argv, "p:e:")) != -1) {
        switch (opcion) {
        case 'p':
            if (arg_pais != NULL) {
                reportar_error(EINVAL, NOMBRE_PROG ": el flag -p está repetido");
                mostrar_uso();
                return EXIT_FAILURE;
            }
            arg_pais = optarg;
            break;
        case 'e':
            if (arg_porc != NULL) {
                reportar_error(EINVAL, NOMBRE_PROG ": el flag -e está repetido");
                mostrar_uso();
                return EXIT_FAILURE;
            }
            arg_porc = optarg;
            break;
        case '?':
        default:
            if (optopt == 'p' || optopt == 'e') {
                reportar_error(EINVAL,
                               NOMBRE_PROG ": el flag -%c requiere un valor",
                               optopt);
            } else if (isprint(optopt)) {
                reportar_error(EINVAL,
                               NOMBRE_PROG ": flag desconocido -%c", optopt);
            } else {
                reportar_error(EINVAL,
                               NOMBRE_PROG ": flag desconocido (código 0x%x)",
                               (unsigned)optopt);
            }
            mostrar_uso();
            return EXIT_FAILURE;
        }
    }

    /* 2. Argumentos sobrantes (por ejemplo "FR" sin su flag). */
    if (optind < argc) {
        reportar_error(EINVAL,
                       NOMBRE_PROG ": argumento inesperado '%s'", argv[optind]);
        mostrar_uso();
        return EXIT_FAILURE;
    }

    /* 3. Flags obligatorios. */
    if (arg_pais == NULL) {
        reportar_error(EINVAL, NOMBRE_PROG ": falta el flag obligatorio -p");
        mostrar_uso();
        return EXIT_FAILURE;
    }
    if (arg_porc == NULL) {
        reportar_error(EINVAL, NOMBRE_PROG ": falta el flag obligatorio -e");
        mostrar_uso();
        return EXIT_FAILURE;
    }

    /* 4. Validación de valores. */
    id_pais = buscar_pais(arg_pais);
    if (id_pais < 0) {
        reportar_error(EINVAL,
                       NOMBRE_PROG ": país '%s' inválido (use ES, FR o IT)",
                       arg_pais);
        mostrar_uso();
        return EXIT_FAILURE;
    }
    if (parsear_porcentaje(arg_porc, &porcentaje) != 0) {
        mostrar_uso();
        return EXIT_FAILURE;
    }

    /* 5. Nombre del fichero: encuestas_XX.csv (con verificación de tamaño). */
    escritos = snprintf(nombre, sizeof(nombre), "encuestas_%s.csv",
                        SIGLAS[id_pais]);
    if (escritos < 0 || (size_t)escritos >= sizeof(nombre)) {
        reportar_error(ENAMETOOLONG,
                       NOMBRE_PROG ": no se pudo construir el nombre del fichero");
        return EXIT_FAILURE;
    }

    /* 6. Cantidad por sector y total. */
    for (int s = 0; s < NUM_SECTORES; s++) {
        cantidades[s] = calcular_cantidad(MAXIMOS[id_pais][s], porcentaje);
        total += cantidades[s];
    }

    /* 7. Semilla única para los números aleatorios (se llama UNA sola vez). */
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    /* 8. Mensaje inicial y generación del fichero. */
    printf("Generando fichero %s para país %s con %ld%% de encuestas\n",
           nombre, SIGLAS[id_pais], porcentaje);

    if (generar_csv(nombre, id_pais, cantidades) != 0) {
        return EXIT_FAILURE;
    }

    /* 9. Resumen final. */
    printf("Máximos por sector:\n");
    for (int s = 0; s < NUM_SECTORES; s++) {
        printf("  %-12s : %ld/%ld encuestas (%ld%% del máximo)\n",
               SECTOR_CONSOLA[s], cantidades[s], MAXIMOS[id_pais][s],
               porcentaje);
    }
    printf("\nTotal encuestas: %ld\n", total);

    if (fflush(stdout) != 0) {
        reportar_error(0, NOMBRE_PROG ": error al escribir en la salida estándar");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
