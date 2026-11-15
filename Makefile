# Makefile - Proyecto ISE (Sistemas Operativos 2026-30)
# Uso: make        -> compila los tres programas
#      make clean  -> borra objetos y ejecutables

CC = gcc
CFLAGS = -Wall

all: generadorCSV agentePais servidorISE

generadorCSV: generadorCSV.o ise.o
	$(CC) generadorCSV.o ise.o -o generadorCSV

agentePais: agentePais.o ise.o
	$(CC) agentePais.o ise.o -o agentePais

servidorISE: servidorISE.o ise.o
	$(CC) servidorISE.o ise.o -o servidorISE -lpthread

generadorCSV.o: generadorCSV.c ise.h
	$(CC) $(CFLAGS) -c generadorCSV.c

agentePais.o: agentePais.c ise.h
	$(CC) $(CFLAGS) -c agentePais.c

servidorISE.o: servidorISE.c ise.h
	$(CC) $(CFLAGS) -c servidorISE.c

ise.o: ise.c ise.h
	$(CC) $(CFLAGS) -c ise.c

clean:
	rm -f *.o generadorCSV agentePais servidorISE
