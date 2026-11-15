/*
 * ise.h - Definiciones compartidas del proyecto ISE
 * (generadorCSV, agentePais y servidorISE)
 */
#ifndef ISE_H
#define ISE_H

#include <sys/types.h>

#define NUM_PAISES   3
#define NUM_SECTORES 5
#define TAM_NOMBRE   64

/* Tipos de mensaje que viajan por el pipe nominal */
#define MSG_CONEXION 1   /* el agente se presenta al servidor        */
#define MSG_ENCUESTA 2   /* una encuesta (sector + sentimiento)      */
#define MSG_FIN      3   /* el agente termino de enviar              */
#define MSG_TIEMPO   4   /* lo envia el hilo temporizador del server */

/*
 * Mensaje de tamano fijo. Como mide menos que PIPE_BUF, cada write()
 * es atomico y los mensajes de varios agentes no se mezclan en el pipe.
 */
typedef struct {
    int    tipo;
    pid_t  pid;                  /* identifica al agente que envia */
    char   pais[3];              /* ES, FR o IT                    */
    char   nombre[TAM_NOMBRE];   /* fichero CSV (nombre del agente) */
    int    sector;               /* 0..4 (ID_Sector del CSV)       */
    double sentimiento;          /* 0.00 .. 100.00                 */
} mensaje_t;

/* Tablas definidas en ise.c */
extern const char *SIGLAS[NUM_PAISES];
extern const char *SECTORES_CSV[NUM_SECTORES];
extern const char *SECTORES[NUM_SECTORES];
extern const int PESOS[NUM_SECTORES];
extern const int MAXIMOS[NUM_PAISES][NUM_SECTORES];

int es_numero(const char *texto);
const char *recomendacion(double ise);

#endif
