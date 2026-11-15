/*
 * ise.c - Tablas y funciones comunes del proyecto ISE
 */
#include <ctype.h>
#include "ise.h"

/* El indice de cada sigla es el ID_Pais del CSV (ES=0, FR=1, IT=2) */
const char *SIGLAS[NUM_PAISES] = { "ES", "FR", "IT" };

/* Nombres de sector: en el CSV van sin tilde, en consola con tilde */
const char *SECTORES_CSV[NUM_SECTORES] = {
    "Industria", "Servicios", "Consumidores", "Comercio", "Construccion"
};
const char *SECTORES[NUM_SECTORES] = {
    "Industria", "Servicios", "Consumidores", "Comercio", "Construcción"
};

/* Peso de cada sector en el ISE (en %) */
const int PESOS[NUM_SECTORES] = { 40, 30, 20, 5, 5 };

/* Tabla 1 del enunciado: maximo de encuestas por pais y sector */
const int MAXIMOS[NUM_PAISES][NUM_SECTORES] = {
    { 1760, 1430, 2020, 1080,   290 },   /* ES */
    { 3850, 4240, 1670, 2860, 24020 },   /* FR */
    { 5440, 3100, 2000, 1560,  1000 }    /* IT */
};

/* Devuelve 1 si el texto tiene solo digitos (y al menos uno) */
int es_numero(const char *texto)
{
    int i;

    if (texto[0] == '\0')
        return 0;
    for (i = 0; texto[i] != '\0'; i++) {
        if (!isdigit((unsigned char) texto[i]))
            return 0;
    }
    return 1;
}

/* >50 Vender, <50 Comprar, =50 Neutro (se compara con 2 decimales) */
const char *recomendacion(double ise)
{
    long valor = (long) (ise * 100.0 + 0.5);

    if (valor > 5000)
        return "Vender";
    if (valor < 5000)
        return "Comprar";
    return "Neutro";
}
