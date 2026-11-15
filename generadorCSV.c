/*
 * generadorCSV.c - Generador de encuestas por pais
 *
 * Uso: ./generadorCSV -p <pais> -e <porcentaje>   (flags en cualquier orden)
 * Ejemplo: ./generadorCSV -p FR -e 75  ->  crea encuestas_FR.csv
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include "ise.h"

static void uso(void)
{
    fprintf(stderr, "Uso: ./generadorCSV -p <pais> -e <porcentaje>\n");
    fprintf(stderr, "  -p  sigla del pais: ES, FR o IT\n");
    fprintf(stderr, "  -e  porcentaje de encuestas a generar (1 a 100)\n");
}

/* Devuelve el indice del pais (0..2) o -1 si no existe */
static int buscar_pais(char *sigla)
{
    int i;

    for (i = 0; sigla[i] != '\0'; i++)
        sigla[i] = toupper((unsigned char) sigla[i]);
    for (i = 0; i < NUM_PAISES; i++) {
        if (strcmp(sigla, SIGLAS[i]) == 0)
            return i;
    }
    return -1;
}

int main(int argc, char *argv[])
{
    char *arg_pais = NULL, *arg_porc = NULL;
    char nombre[TAM_NOMBRE];
    int cantidad[NUM_SECTORES];
    int pais, porcentaje, total = 0, id = 0, s, i, valor;
    FILE *fp;

    /* Se esperan 2 flags con su valor: el orden puede cambiar */
    if (argc != 5) {
        uso();
        exit(1);
    }
    for (i = 1; i < argc; i += 2) {
        if (strcmp(argv[i], "-p") == 0)
            arg_pais = argv[i + 1];
        else if (strcmp(argv[i], "-e") == 0)
            arg_porc = argv[i + 1];
        else {
            fprintf(stderr, "Error: flag desconocido '%s'\n", argv[i]);
            uso();
            exit(1);
        }
    }
    if (arg_pais == NULL || arg_porc == NULL) {   /* p. ej. -p repetido */
        fprintf(stderr, "Error: faltan los flags -p y/o -e\n");
        uso();
        exit(1);
    }

    pais = buscar_pais(arg_pais);
    if (pais < 0) {
        fprintf(stderr, "Error: pais '%s' invalido (use ES, FR o IT)\n", arg_pais);
        exit(1);
    }
    if (!es_numero(arg_porc) || atoi(arg_porc) < 1 || atoi(arg_porc) > 100) {
        fprintf(stderr, "Error: porcentaje '%s' invalido (entero de 1 a 100)\n", arg_porc);
        exit(1);
    }
    porcentaje = atoi(arg_porc);

    /* Cantidad por sector, redondeando al entero mas cercano (.5 sube).
     * Ej: 3850 * 75% = 2887.5 -> 2888, igual que en el enunciado */
    for (s = 0; s < NUM_SECTORES; s++) {
        cantidad[s] = (MAXIMOS[pais][s] * porcentaje + 50) / 100;
        total += cantidad[s];
    }

    sprintf(nombre, "encuestas_%s.csv", SIGLAS[pais]);
    printf("Generando fichero %s para país %s con %d%% de encuestas\n",
           nombre, SIGLAS[pais], porcentaje);

    fp = fopen(nombre, "w");
    if (fp == NULL) {
        perror("Error al crear el fichero");
        exit(1);
    }

    srand(time(NULL) ^ getpid());
    fprintf(fp, "ID_Encuesta,ID_Pais,Pais,Sector,ID_Sector,Sentimiento\n");
    for (s = 0; s < NUM_SECTORES; s++) {
        for (i = 0; i < cantidad[s]; i++) {
            id++;
            valor = rand() % 10001;   /* 0..10000 -> 0.00% .. 100.00% */
            fprintf(fp, "%d,%d,%s,%s,%d,%.2f\n", id, pais, SIGLAS[pais],
                    SECTORES_CSV[s], s, valor / 100.0);
        }
    }

    /* fclose vacia el buffer: si falla, el fichero puede quedar incompleto */
    if (fclose(fp) != 0) {
        perror("Error al escribir el fichero");
        remove(nombre);
        exit(1);
    }

    printf("Máximos por sector:\n");
    for (s = 0; s < NUM_SECTORES; s++) {
        /* %-12s cuenta bytes: "Construcción" (con tilde) ya ocupa 13 */
        printf("  %-12s : %d/%d encuestas (%d%% del máximo)\n",
               SECTORES[s], cantidad[s], MAXIMOS[pais][s], porcentaje);
    }
    printf("\nTotal encuestas: %d\n", total);
    return 0;
}
