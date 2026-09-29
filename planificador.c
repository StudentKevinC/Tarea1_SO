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
    char dependencias[MAX_DEPENDENCIAS][MAX_ID];
    int cant_dependencias;
    int estado;
    pid_t pid;
    int fd[2];
} Actividad;

Actividad actividades[MAX_ACTIVIDADES];
int cantidad = 0;

volatile sig_atomic_t ctrl_c = 0;


// quita espacios al principio y al final
char *limpiar(char *texto) {
    while (isspace((unsigned char)*texto)) {
        texto++;
    }

    if (*texto == '\0') {
        return texto;
    }

    char *final = texto + strlen(texto) - 1;

    while (final > texto && isspace((unsigned char)*final)) {
        final--;
    }

    final[1] = '\0';

    return texto;
}


// busca una actividad por su id
int buscar_id(const char *id) {
    for (int i = 0; i < cantidad; i++) {
        if (strcmp(actividades[i].id, id) == 0) {
            return i;
        }
    }

    return -1;
}


// busca una actividad usando el pid
int buscar_pid(pid_t pid) {
    for (int i = 0; i < cantidad; i++) {
        if (actividades[i].pid == pid) {
            return i;
        }
    }

    return -1;
}


// lee las actividades desde plan.txt
void leer_plan(const char *nombre_archivo) {
    FILE *archivo = fopen(nombre_archivo, "r");

    if (archivo == NULL) {
        perror("No se pudo abrir el archivo");
        exit(EXIT_FAILURE);
    }

    char linea[4096];

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

        char *tiempo = limpiar(resto);
        char *deps = limpiar(p3 + 1);

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

        // si no viene tiempo se genera uno
        if (*tiempo == '\0') {
            a->tiempo = 100 + rand() % 4901;
        } else {
            char *fin;
            long valor = strtol(tiempo, &fin, 10);

            if (*limpiar(fin) != '\0' || valor <= 0) {
                fprintf(stderr, "Tiempo incorrecto en %s\n", a->id);
                fclose(archivo);
                exit(EXIT_FAILURE);
            }

            a->tiempo = (int)valor;
        }

        a->estado = PENDIENTE;
        a->pid = -1;
        a->fd[0] = -1;
        a->fd[1] = -1;
        a->cant_dependencias = 0;

        // guarda las dependencias separadas por coma
        if (*deps != '\0') {
            char copia[4096];

            strncpy(copia, deps, sizeof(copia) - 1);
            copia[sizeof(copia) - 1] = '\0';

            char *guardar = NULL;
            char *dep = strtok_r(copia, ",", &guardar);

            while (dep != NULL) {
                dep = limpiar(dep);

                if (*dep != '\0') {
                    if (a->cant_dependencias >= MAX_DEPENDENCIAS) {
                        fprintf(stderr, "Demasiadas dependencias\n");
                        fclose(archivo);
                        exit(EXIT_FAILURE);
                    }

                    strncpy(
                        a->dependencias[a->cant_dependencias],
                        dep,
                        MAX_ID - 1
                    );

                    a->cant_dependencias++;
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


// revisa que todas las dependencias existan
void revisar_dependencias(void) {
    for (int i = 0; i < cantidad; i++) {
        for (int j = 0; j < actividades[i].cant_dependencias; j++) {
            char *dep = actividades[i].dependencias[j];

            if (strcmp(dep, actividades[i].id) == 0) {
                fprintf(
                    stderr,
                    "La actividad %s depende de si misma\n",
                    actividades[i].id
                );

                exit(EXIT_FAILURE);
            }

            if (buscar_id(dep) == -1) {
                fprintf(
                    stderr,
                    "La dependencia %s no existe\n",
                    dep
                );

                exit(EXIT_FAILURE);
            }
        }
    }
}


// revisa si una actividad ya puede comenzar
int puede_empezar(int pos) {
    Actividad *a = &actividades[pos];

    if (a->estado != PENDIENTE) {
        return 0;
    }

    for (int i = 0; i < a->cant_dependencias; i++) {
        int dep = buscar_id(a->dependencias[i]);

        if (dep == -1) {
            return 0;
        }

        if (actividades[dep].estado != TERMINADA) {
            return 0;
        }
    }

    return 1;
}


// aborta las actividades que dependen de una que fallo
int revisar_abortadas(void) {
    int cambios = 0;

    for (int i = 0; i < cantidad; i++) {
        Actividad *a = &actividades[i];

        if (a->estado != PENDIENTE) {
            continue;
        }

        for (int j = 0; j < a->cant_dependencias; j++) {
            int dep = buscar_id(a->dependencias[j]);

            if (dep >= 0 &&
                (actividades[dep].estado == FALLIDA ||
                 actividades[dep].estado == ABORTADA)) {

                a->estado = ABORTADA;

                printf(
                    "[ABORTADA] %s depende de %s\n",
                    a->nombre,
                    actividades[dep].nombre
                );

                cambios++;
                break;
            }
        }
    }

    return cambios;
}


// solo avisa que se presiono ctrl+c
void recibir_sigint(int sig) {
    (void)sig;
    ctrl_c = 1;
}


// termina los procesos que siguen funcionando
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


// crea el proceso de una actividad
int iniciar_actividad(int pos) {
    Actividad *a = &actividades[pos];

    if (pipe(a->fd) == -1) {
        perror("pipe");
        return -1;
    }

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");

        close(a->fd[0]);
        close(a->fd[1]);

        a->fd[0] = -1;
        a->fd[1] = -1;

        return -1;
    }

    if (pid == 0) {
        close(a->fd[0]);

        // sirve para probar el manejo de errores
        char *fallar = getenv("FALLAR_ID");

        if (fallar != NULL && strcmp(fallar, a->id) == 0) {
            printf(
                "[FALLO] PID=%ld | %s\n",
                (long)getpid(),
                a->nombre
            );

            fflush(stdout);
            close(a->fd[1]);

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
                close(a->fd[1]);
                _exit(EXIT_FAILURE);
            }
        }

        char mensaje[MAX_MENSAJE];

        snprintf(
            mensaje,
            sizeof(mensaje),
            "%s termino",
            a->nombre
        );

        if (write(a->fd[1], mensaje, strlen(mensaje) + 1) == -1) {
            close(a->fd[1]);
            _exit(EXIT_FAILURE);
        }

        close(a->fd[1]);

        printf(
            "[FIN] PID=%ld | %s\n",
            (long)getpid(),
            a->nombre
        );

        fflush(stdout);

        _exit(EXIT_SUCCESS);
    }

    // el padre solo lee el pipe
    close(a->fd[1]);
    a->fd[1] = -1;

    a->pid = pid;
    a->estado = EJECUTANDO;

    return 0;
}


// guarda el resultado de un hijo que termino
void terminar_actividad(pid_t pid, int status) {
    int pos = buscar_pid(pid);

    if (pos == -1) {
        return;
    }

    Actividad *a = &actividades[pos];

    char mensaje[MAX_MENSAJE];

    ssize_t leidos = read(
        a->fd[0],
        mensaje,
        sizeof(mensaje) - 1
    );

    if (leidos > 0) {
        mensaje[leidos] = '\0';
        printf("[PIPE] %s\n", mensaje);
    }

    if (a->fd[0] >= 0) {
        close(a->fd[0]);
        a->fd[0] = -1;
    }

    if (WIFEXITED(status) &&
        WEXITSTATUS(status) == EXIT_SUCCESS) {

        a->estado = TERMINADA;
    } else {
        a->estado = FALLIDA;
        printf("[FALLO] %s\n", a->nombre);
    }

    a->pid = -1;
}


// revisa si ya no queda nada por ejecutar
int termino_plan(void) {
    for (int i = 0; i < cantidad; i++) {
        if (actividades[i].estado == PENDIENTE ||
            actividades[i].estado == EJECUTANDO) {

            return 0;
        }
    }

    return 1;
}


// parte principal del planificador
void ejecutar_plan(int k) {
    int ejecutando = 0;

    while (!termino_plan()) {
        if (ctrl_c) {
            cancelar_todo();

            int status;
            pid_t pid;

            while ((pid = waitpid(-1, &status, 0)) > 0) {
                int pos = buscar_pid(pid);

                if (pos >= 0) {
                    actividades[pos].estado = ABORTADA;

                    if (actividades[pos].fd[0] >= 0) {
                        close(actividades[pos].fd[0]);
                        actividades[pos].fd[0] = -1;
                    }

                    actividades[pos].pid = -1;
                }
            }

            return;
        }

        // sigue propagando los fallos si hay mas dependientes
        while (revisar_abortadas() > 0) {
        }

        // inicia actividades hasta llegar al limite k
        for (int i = 0; i < cantidad && ejecutando < k; i++) {
            if (puede_empezar(i)) {
                if (iniciar_actividad(i) == 0) {
                    ejecutando++;
                } else {
                    actividades[i].estado = FALLIDA;
                }
            }
        }

        // espera a que termine algun hijo
        if (ejecutando > 0) {
            int status;
            pid_t pid = waitpid(-1, &status, 0);

            if (pid > 0) {
                terminar_actividad(pid, status);
                ejecutando--;
            } else if (pid == -1 && errno == EINTR) {
                continue;
            } else if (pid == -1) {
                perror("waitpid");
                break;
            }

            continue;
        }

        // si quedan pendientes y nadie puede empezar puede haber un ciclo
        int pendientes = 0;

        for (int i = 0; i < cantidad; i++) {
            if (actividades[i].estado == PENDIENTE) {
                pendientes = 1;
                break;
            }
        }

        if (pendientes) {
            fprintf(
                stderr,
                "No se puede continuar, puede haber un ciclo\n"
            );

            break;
        }
    }
}


char *texto_estado(int estado) {
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


// muestra como termino cada actividad
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
    revisar_dependencias();

    printf("Actividades cargadas: %d\n", cantidad);
    printf("Limite de procesos: %d\n\n", k);

    ejecutar_plan(k);

    mostrar_resumen();

    return EXIT_SUCCESS;
}