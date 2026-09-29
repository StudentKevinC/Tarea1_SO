#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>

#define MAX_ACTIVIDADES 10000
#define MAX_DEPENDENCIAS 100
#define MAX_ID 50
#define MAX_NOMBRE 100
#define MAX_LINEA 8192
#define MAX_MENSAJE 256

#define PENDIENTE 0
#define EJECUTANDO 1
#define TERMINADA 2
#define FALLIDA 3
#define ABORTADA 4

typedef struct {
    char id[MAX_ID];
    char nombre[MAX_NOMBRE];
    int tiempo;

    char dep_id[MAX_DEPENDENCIAS][MAX_ID];
    int deps[MAX_DEPENDENCIAS];
    int cant_deps;
    int faltan;

    int estado;
    pid_t pid;
} Actividad;

typedef struct {
    int destino;
    int siguiente;
} Arista;

Actividad actividades[MAX_ACTIVIDADES];

Arista *aristas = NULL;
int *primera_arista = NULL;

int cola[MAX_ACTIVIDADES];
int inicio_cola = 0;
int fin_cola = 0;

int cantidad = 0;
int cant_aristas = 0;

volatile sig_atomic_t ctrl_c = 0;


// quita espacios al principio y al final
char *limpiar(char *texto) {
    while (isspace((unsigned char)*texto)) {
        texto++;
    }

    if (*texto == '\0') {
        return texto;
    }

    char *fin = texto + strlen(texto) - 1;

    while (fin > texto && isspace((unsigned char)*fin)) {
        fin--;
    }

    fin[1] = '\0';

    return texto;
}


// busca una actividad por id
int buscar_id(const char *id) {
    for (int i = 0; i < cantidad; i++) {
        if (strcmp(actividades[i].id, id) == 0) {
            return i;
        }
    }

    return -1;
}


// busca una actividad por pid
int buscar_pid(pid_t pid) {
    for (int i = 0; i < cantidad; i++) {
        if (actividades[i].pid == pid) {
            return i;
        }
    }

    return -1;
}


// agrega una actividad a la cola de listas
void agregar_cola(int pos) {
    if (fin_cola < MAX_ACTIVIDADES) {
        cola[fin_cola++] = pos;
    }
}


// saca una actividad de la cola
int sacar_cola(void) {
    if (inicio_cola >= fin_cola) {
        return -1;
    }

    return cola[inicio_cola++];
}


// lee el archivo plan.txt
void leer_plan(const char *nombre_archivo) {
    FILE *archivo = fopen(nombre_archivo, "r");

    if (archivo == NULL) {
        perror("No se pudo abrir el archivo");
        exit(EXIT_FAILURE);
    }

    char linea[MAX_LINEA];

    while (fgets(linea, sizeof(linea), archivo) != NULL) {
        linea[strcspn(linea, "\r\n")] = '\0';

        char *texto = limpiar(linea);

        if (*texto == '\0') {
            continue;
        }

        if (cantidad >= MAX_ACTIVIDADES) {
            fprintf(stderr, "Se supero el maximo de actividades\n");
            fclose(archivo);
            exit(EXIT_FAILURE);
        }

        char *p1 = strchr(texto, ':');

        if (p1 == NULL) {
            fprintf(stderr, "Linea incorrecta\n");
            fclose(archivo);
            exit(EXIT_FAILURE);
        }

        *p1 = '\0';

        char *id = limpiar(texto);
        char *resto = p1 + 1;

        char *p2 = strchr(resto, ':');

        if (p2 == NULL) {
            fprintf(stderr, "Linea incorrecta\n");
            fclose(archivo);
            exit(EXIT_FAILURE);
        }

        *p2 = '\0';

        char *nombre = limpiar(resto);
        resto = p2 + 1;

        char *p3 = strchr(resto, ':');

        if (p3 == NULL) {
            fprintf(stderr, "Linea incorrecta\n");
            fclose(archivo);
            exit(EXIT_FAILURE);
        }

        *p3 = '\0';

        char *tiempo_txt = limpiar(resto);
        char *deps_txt = limpiar(p3 + 1);

        if (*id == '\0' || *nombre == '\0') {
            fprintf(stderr, "ID o nombre vacio\n");
            fclose(archivo);
            exit(EXIT_FAILURE);
        }

        if (buscar_id(id) != -1) {
            fprintf(stderr, "ID repetido: %s\n", id);
            fclose(archivo);
            exit(EXIT_FAILURE);
        }

        Actividad *a = &actividades[cantidad];

        memset(a, 0, sizeof(Actividad));

        strncpy(a->id, id, MAX_ID - 1);
        strncpy(a->nombre, nombre, MAX_NOMBRE - 1);

        if (*tiempo_txt == '\0') {
            a->tiempo = 100 + rand() % 4901;
        } else {
            char *fin;
            long valor = strtol(tiempo_txt, &fin, 10);

            if (*limpiar(fin) != '\0' || valor <= 0) {
                fprintf(stderr, "Tiempo incorrecto en %s\n", a->id);
                fclose(archivo);
                exit(EXIT_FAILURE);
            }

            a->tiempo = (int)valor;
        }

        a->estado = PENDIENTE;
        a->pid = -1;
        a->cant_deps = 0;

        // guarda primero los id de las dependencias
        if (*deps_txt != '\0') {
            char copia[MAX_LINEA];

            strncpy(copia, deps_txt, sizeof(copia) - 1);
            copia[sizeof(copia) - 1] = '\0';

            char *guardar = NULL;
            char *dep = strtok_r(copia, ",", &guardar);

            while (dep != NULL) {
                dep = limpiar(dep);

                if (*dep != '\0') {
                    if (a->cant_deps >= MAX_DEPENDENCIAS) {
                        fprintf(stderr, "Demasiadas dependencias\n");
                        fclose(archivo);
                        exit(EXIT_FAILURE);
                    }

                    strncpy(
                        a->dep_id[a->cant_deps],
                        dep,
                        MAX_ID - 1
                    );

                    a->cant_deps++;
                }

                dep = strtok_r(NULL, ",", &guardar);
            }
        }

        cantidad++;
    }

    fclose(archivo);

    if (cantidad == 0) {
        fprintf(stderr, "El plan esta vacio\n");
        exit(EXIT_FAILURE);
    }
}


// convierte las dependencias a posiciones numericas
void preparar_dependencias(void) {
    int total = 0;

    for (int i = 0; i < cantidad; i++) {
        total += actividades[i].cant_deps;
    }

    aristas = malloc(sizeof(Arista) * (total > 0 ? total : 1));
    primera_arista = malloc(sizeof(int) * cantidad);

    if (aristas == NULL || primera_arista == NULL) {
        fprintf(stderr, "No hay memoria suficiente\n");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < cantidad; i++) {
        primera_arista[i] = -1;
    }

    for (int i = 0; i < cantidad; i++) {
        Actividad *a = &actividades[i];

        for (int j = 0; j < a->cant_deps; j++) {
            int dep = buscar_id(a->dep_id[j]);

            if (dep == -1) {
                fprintf(
                    stderr,
                    "La dependencia %s no existe\n",
                    a->dep_id[j]
                );

                exit(EXIT_FAILURE);
            }

            if (dep == i) {
                fprintf(
                    stderr,
                    "La actividad %s depende de si misma\n",
                    a->id
                );

                exit(EXIT_FAILURE);
            }

            a->deps[j] = dep;

            // guarda quien depende de esta actividad
            aristas[cant_aristas].destino = i;
            aristas[cant_aristas].siguiente = primera_arista[dep];
            primera_arista[dep] = cant_aristas;

            cant_aristas++;
        }

        a->faltan = a->cant_deps;
    }

    // las que no tienen dependencias pueden partir
    for (int i = 0; i < cantidad; i++) {
        if (actividades[i].cant_deps == 0) {
            agregar_cola(i);
        }
    }
}


// recibe ctrl+c
void recibir_sigint(int sig) {
    (void)sig;
    ctrl_c = 1;
}


// aborta una rama cuando una actividad falla
void abortar_rama(int pos) {
    int arista = primera_arista[pos];

    while (arista != -1) {
        int hijo = aristas[arista].destino;

        if (actividades[hijo].estado == PENDIENTE) {
            actividades[hijo].estado = ABORTADA;

            printf(
                "[ABORTADA] %s depende de %s\n",
                actividades[hijo].nombre,
                actividades[pos].nombre
            );

            abortar_rama(hijo);
        }

        arista = aristas[arista].siguiente;
    }
}


// avisa a los dependientes que una actividad termino
void avisar_dependientes(int pos) {
    int arista = primera_arista[pos];

    while (arista != -1) {
        int hijo = aristas[arista].destino;

        if (actividades[hijo].estado == PENDIENTE) {
            actividades[hijo].faltan--;

            if (actividades[hijo].faltan == 0) {
                agregar_cola(hijo);
            }
        }

        arista = aristas[arista].siguiente;
    }
}


// envia los mensajes de las dependencias por un pipe
void enviar_insumos(int fd, int pos) {
    Actividad *a = &actividades[pos];

    for (int i = 0; i < a->cant_deps; i++) {
        int dep = a->deps[i];

        char mensaje[MAX_MENSAJE];

        int largo = snprintf(
            mensaje,
            sizeof(mensaje),
            "%s termino\n",
            actividades[dep].nombre
        );

        if (largo > 0) {
            if (largo >= MAX_MENSAJE) {
                largo = MAX_MENSAJE - 1;
            }

            if (write(fd, mensaje, (size_t)largo) == -1) {
                break;
            }
        }
    }
}


// crea el proceso de una actividad
int iniciar_actividad(int pos) {
    Actividad *a = &actividades[pos];

    int fd[2];

    if (pipe(fd) == -1) {
        perror("pipe");
        return -1;
    }

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");

        close(fd[0]);
        close(fd[1]);

        return -1;
    }

    if (pid == 0) {
        close(fd[1]);

        // lee los mensajes que vienen de sus dependencias
        char buffer[MAX_MENSAJE];
        ssize_t leidos;

        while ((leidos = read(fd[0], buffer, sizeof(buffer) - 1)) > 0) {
            buffer[leidos] = '\0';

            printf(
                "[PIPE -> %s] %s",
                a->nombre,
                buffer
            );

            fflush(stdout);
        }

        close(fd[0]);

        // se usa solo para probar errores
        char *fallar = getenv("FALLAR_ID");

        if (fallar != NULL && strcmp(fallar, a->id) == 0) {
            printf(
                "[FALLO] PID=%ld | %s\n",
                (long)getpid(),
                a->nombre
            );

            fflush(stdout);
            _exit(EXIT_FAILURE);
        }

        printf(
            "[INICIO] PID=%ld | %s | %d ms\n",
            (long)getpid(),
            a->nombre,
            a->tiempo
        );

        fflush(stdout);

        struct timespec espera;

        espera.tv_sec = a->tiempo / 1000;
        espera.tv_nsec = (long)(a->tiempo % 1000) * 1000000L;

        while (nanosleep(&espera, &espera) == -1) {
            if (errno != EINTR) {
                _exit(EXIT_FAILURE);
            }
        }

        printf(
            "[FIN] PID=%ld | %s\n",
            (long)getpid(),
            a->nombre
        );

        fflush(stdout);

        _exit(EXIT_SUCCESS);
    }

    // el padre manda los insumos y cierra el pipe
    close(fd[0]);

    enviar_insumos(fd[1], pos);

    close(fd[1]);

    a->pid = pid;
    a->estado = EJECUTANDO;

    return 0;
}


// procesa un hijo que termino
void procesar_hijo(pid_t pid, int status) {
    int pos = buscar_pid(pid);

    if (pos == -1) {
        return;
    }

    Actividad *a = &actividades[pos];

    a->pid = -1;

    if (WIFEXITED(status) &&
        WEXITSTATUS(status) == EXIT_SUCCESS) {

        a->estado = TERMINADA;
        avisar_dependientes(pos);

    } else {
        a->estado = FALLIDA;

        printf("[FALLO] %s\n", a->nombre);

        abortar_rama(pos);
    }
}


// cancela todo con ctrl+c
void cancelar_todo(void) {
    printf("\nCtrl+C recibido, cancelando actividades...\n");

    for (int i = 0; i < cantidad; i++) {
        if (actividades[i].estado == EJECUTANDO &&
            actividades[i].pid > 0) {

            kill(actividades[i].pid, SIGTERM);
        }

        if (actividades[i].estado == PENDIENTE) {
            actividades[i].estado = ABORTADA;
        }
    }
}


// revisa si quedan actividades pendientes
int hay_pendientes(void) {
    for (int i = 0; i < cantidad; i++) {
        if (actividades[i].estado == PENDIENTE) {
            return 1;
        }
    }

    return 0;
}


// ejecuta el plan
void ejecutar_plan(int k) {
    int ejecutando = 0;
    int terminadas = 0;

    while (terminadas < cantidad) {
        if (ctrl_c) {
            cancelar_todo();

            int status;
            pid_t pid;

            while ((pid = waitpid(-1, &status, 0)) > 0) {
                int pos = buscar_pid(pid);

                if (pos >= 0) {
                    actividades[pos].estado = ABORTADA;
                    actividades[pos].pid = -1;
                }
            }

            return;
        }

        // inicia las actividades que ya estan listas
        while (ejecutando < k) {
            int pos = sacar_cola();

            if (pos == -1) {
                break;
            }

            if (actividades[pos].estado != PENDIENTE) {
                continue;
            }

            if (iniciar_actividad(pos) == 0) {
                ejecutando++;
            } else {
                actividades[pos].estado = FALLIDA;
                abortar_rama(pos);
                terminadas++;
            }
        }

        if (ejecutando > 0) {
            int status;
            pid_t pid = waitpid(-1, &status, 0);

            if (pid > 0) {
                procesar_hijo(pid, status);
                ejecutando--;
                terminadas++;

                continue;
            }

            if (pid == -1 && errno == EINTR) {
                continue;
            }

            if (pid == -1) {
                perror("waitpid");
                break;
            }
        }

        // cuenta abortadas para saber si ya termino todo
        int resueltas = 0;

        for (int i = 0; i < cantidad; i++) {
            if (actividades[i].estado == TERMINADA ||
                actividades[i].estado == FALLIDA ||
                actividades[i].estado == ABORTADA) {

                resueltas++;
            }
        }

        terminadas = resueltas;

        if (ejecutando == 0 &&
            inicio_cola >= fin_cola &&
            hay_pendientes()) {

            fprintf(
                stderr,
                "No se puede continuar, puede haber un ciclo\n"
            );

            break;
        }
    }
}


const char *texto_estado(int estado) {
    switch (estado) {
        case TERMINADA:
            return "TERMINADA";

        case FALLIDA:
            return "FALLIDA";

        case ABORTADA:
            return "ABORTADA";

        case EJECUTANDO:
            return "EJECUTANDO";

        default:
            return "PENDIENTE";
    }
}


// muestra el resultado final
void mostrar_resumen(void) {
    printf("\n===== RESUMEN =====\n");

    for (int i = 0; i < cantidad; i++) {
        printf(
            "%s - %-20s : %s\n",
            actividades[i].id,
            actividades[i].nombre,
            texto_estado(actividades[i].estado)
        );
    }
}


int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(
            stderr,
            "Uso: %s plan.txt K\n",
            argv[0]
        );

        return EXIT_FAILURE;
    }

    char *fin;
    long valor_k = strtol(argv[2], &fin, 10);

    if (*argv[2] == '\0' ||
        *fin != '\0' ||
        valor_k <= 0 ||
        valor_k > MAX_ACTIVIDADES) {

        fprintf(stderr, "K debe ser mayor que 0\n");
        return EXIT_FAILURE;
    }

    int k = (int)valor_k;

    srand((unsigned int)time(NULL));

    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = recibir_sigint;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    leer_plan(argv[1]);
    preparar_dependencias();

    printf("Actividades cargadas: %d\n", cantidad);
    printf("Limite de procesos: %d\n\n", k);

    ejecutar_plan(k);

    mostrar_resumen();

    free(aristas);
    free(primera_arista);

    return EXIT_SUCCESS;
}