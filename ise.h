/* ise.h - Mensaje compartido entre agentePais y servidorISE (via pipe nominal). */
#ifndef ISE_H
#define ISE_H

#include <sys/types.h>

#define NUM_SECTORES 5

enum { MSG_HELLO = 0, MSG_DATO = 1, MSG_FIN = 2 };

/* Tamano fijo (< PIPE_BUF): cada write() es atomico aunque haya varios agentes. */
typedef struct {
    int    tipo;          /* MSG_HELLO, MSG_DATO o MSG_FIN          */
    pid_t  pid;           /* identifica al agente que envia         */
    char   pais[3];       /* sigla del pais, p. ej. "ES"            */
    int    sector;        /* 0..4 (ID_Sector del CSV)               */
    double sentimiento;   /* 0.00 .. 100.00                         */
} mensaje_t;

#endif
