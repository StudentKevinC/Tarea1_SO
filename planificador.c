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

#define MAX_NOMBRE 100
#define MAX_ID 50
#define MAX_DEPENDENCIAS 100
#define MAX_ACTIVIDADES 10000
#define MAX_MENSAJE 256

#define PENDIENTE 0
#define EJECUTANDO 1
#define TERMINADA 2
#define FALLIDA 3
#define ABORTADA 4

typedef struct {
    char id[MAX_ID];
    char nombre[MAX_NOMBRE];

    int tiempo_ms;

    char dependencias[MAX_DEPENDENCIAS][MAX_ID];
    int num_dependencias;

    int estado;
    pid_t pid;

    int pipe_fd[2];
} Actividad;

static Actividad actividades[MAX_ACTIVIDADES];
static int num_actividades = 0;

static volatile sig_atomic_t sigint_recibido = 0;

/* -------------------------------------------------- */
/* UTILIDADES                                         */
/* -------------------------------------------------- */

static char *trim(char *str)
{
    char *fin;

    while (isspace((unsigned char)*str)) {
        str++;
    }

    if (*str == '\0') {
        return str;
    }

    fin = str + strlen(str) - 1;

    while (fin > str && isspace((unsigned char)*fin)) {
        fin--;
    }

    fin[1] = '\0';

    return str;
}

static int buscar_actividad(const char *id)
{
    for (int i = 0; i < num_actividades; i++) {
        if (strcmp(actividades[i].id, id) == 0) {
            return i;
        }
    }

    return -1;
}

static int buscar_por_pid(pid_t pid)
{
    for (int i = 0; i < num_actividades; i++) {
        if (actividades[i].pid == pid) {
            return i;
        }
    }

    return -1;
}

/* -------------------------------------------------- */
/* LECTURA DE PLAN.TXT                                */
/* -------------------------------------------------- */

static void leer_plan(const char *nombre_archivo)
{
    FILE *archivo = fopen(nombre_archivo, "r");

    if (archivo == NULL) {
        perror("Error al abrir plan.txt");
        exit(EXIT_FAILURE);
    }

    char linea[4096];

    while (fgets(linea, sizeof(linea), archivo) != NULL) {

        linea[strcspn(linea, "\r\n")] = '\0';

        char *linea_limpia = trim(linea);

        if (*linea_limpia == '\0') {
            continue;
        }

        if (num_actividades >= MAX_ACTIVIDADES) {
            fprintf(stderr, "Error: se supero el maximo de actividades.\n");
            fclose(archivo);
            exit(EXIT_FAILURE);
        }

        /*
         * Se separan manualmente los 4 campos para conservar
         * correctamente un tiempo vacio o dependencias vacias.
         */

        char *sep1 = strchr(linea_limpia, ':');

        if (sep1 == NULL) {
            fprintf(stderr, "Linea invalida: %s\n", linea_limpia);
            fclose(archivo);
            exit(EXIT_FAILURE);
        }

        *sep1 = '\0';

        char *campo1 = trim(linea_limpia);
        char *resto = sep1 + 1;

        char *sep2 = strchr(resto, ':');

        if (sep2 == NULL) {
            fprintf(stderr, "Linea invalida.\n");
            fclose(archivo);
            exit(EXIT_FAILURE);
        }

        *sep2 = '\0';

        char *campo2 = trim(resto);
        resto = sep2 + 1;

        char *sep3 = strchr(resto, ':');

        if (sep3 == NULL) {
            fprintf(stderr, "Linea invalida.\n");
            fclose(archivo);
            exit(EXIT_FAILURE);
        }

        *sep3 = '\0';

        char *campo3 = trim(resto);
        char *campo4 = trim(sep3 + 1);

        if (*campo1 == '\0' || *campo2 == '\0') {
            fprintf(stderr, "Error: ID o nombre vacio.\n");
            fclose(archivo);
            exit(EXIT_FAILURE);
        }

        if (buscar_actividad(campo1) != -1) {
            fprintf(stderr, "Error: ID duplicado: %s\n", campo1);
            fclose(archivo);
            exit(EXIT_FAILURE);
        }

        Actividad *a = &actividades[num_actividades];

        memset(a, 0, sizeof(*a));

        strncpy(a->id, campo1, MAX_ID - 1);
        a->id[MAX_ID - 1] = '\0';

        strncpy(a->nombre, campo2, MAX_NOMBRE - 1);
        a->nombre[MAX_NOMBRE - 1] = '\0';

        if (*campo3 == '\0') {
            a->tiempo_ms = 100 + rand() % 4901;
        } else {
            char *fin_numero = NULL;

            long tiempo = strtol(campo3, &fin_numero, 10);

            if (*trim(fin_numero) != '\0' || tiempo <= 0) {
                fprintf(
                    stderr,
                    "Error: tiempo invalido en actividad %s.\n",
                    a->id
                );

                fclose(archivo);
                exit(EXIT_FAILURE);
            }

            a->tiempo_ms = (int)tiempo;
        }

        a->estado = PENDIENTE;
        a->pid = -1;
        a->pipe_fd[0] = -1;
        a->pipe_fd[1] = -1;
        a->num_dependencias = 0;

        if (*campo4 != '\0') {

            char dependencias[4096];

            strncpy(
                dependencias,
                campo4,
                sizeof(dependencias) - 1
            );

            dependencias[sizeof(dependencias) - 1] = '\0';

            char *saveptr = NULL;

            char *dep = strtok_r(
                dependencias,
                ",",
                &saveptr
            );

            while (dep != NULL) {

                dep = trim(dep);

                if (*dep != '\0') {

                    if (a->num_dependencias >= MAX_DEPENDENCIAS) {
                        fprintf(
                            stderr,
                            "Error: demasiadas dependencias en %s.\n",
                            a->id
                        );

                        fclose(archivo);
                        exit(EXIT_FAILURE);
                    }

                    strncpy(
                        a->dependencias[a->num_dependencias],
                        dep,
                        MAX_ID - 1
                    );

                    a->dependencias[a->num_dependencias][MAX_ID - 1] =
                        '\0';

                    a->num_dependencias++;
                }

                dep = strtok_r(NULL, ",", &saveptr);
            }
        }

        num_actividades++;
    }

    fclose(archivo);

    if (num_actividades == 0) {
        fprintf(stderr, "Error: el plan esta vacio.\n");
        exit(EXIT_FAILURE);
    }
}

/* -------------------------------------------------- */
/* VALIDACION                                         */
/* -------------------------------------------------- */

static void validar_dependencias(void)
{
    for (int i = 0; i < num_actividades; i++) {

        for (int j = 0;
             j < actividades[i].num_dependencias;
             j++) {

            const char *dep =
                actividades[i].dependencias[j];

            if (strcmp(dep, actividades[i].id) == 0) {
                fprintf(
                    stderr,
                    "Error: %s depende de si misma.\n",
                    actividades[i].id
                );

                exit(EXIT_FAILURE);
            }

            if (buscar_actividad(dep) == -1) {
                fprintf(
                    stderr,
                    "Error: actividad %s depende del ID inexistente %s.\n",
                    actividades[i].id,
                    dep
                );

                exit(EXIT_FAILURE);
            }
        }
    }
}

/* -------------------------------------------------- */
/* PROPAGACION DE ABORTOS                             */
/* -------------------------------------------------- */

static int actualizar_abortadas(void)
{
    int cambios = 0;

    for (int i = 0; i < num_actividades; i++) {

        Actividad *a = &actividades[i];

        if (a->estado != PENDIENTE) {
            continue;
        }

        for (int j = 0;
             j < a->num_dependencias;
             j++) {

            int dep =
                buscar_actividad(a->dependencias[j]);

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

static int actividad_lista(int indice)
{
    Actividad *a = &actividades[indice];

    if (a->estado != PENDIENTE) {
        return 0;
    }

    for (int i = 0; i < a->num_dependencias; i++) {

        int dep =
            buscar_actividad(a->dependencias[i]);

        if (dep < 0) {
            return 0;
        }

        if (actividades[dep].estado != TERMINADA) {
            return 0;
        }
    }

    return 1;
}

/* -------------------------------------------------- */
/* SIGNAL SIGINT                                      */
/* -------------------------------------------------- */

static void manejar_sigint(int sig)
{
    (void)sig;
    sigint_recibido = 1;
}

static void abortar_todo(void)
{
    printf("\n[SIGINT] Abortando todas las actividades...\n");

    for (int i = 0; i < num_actividades; i++) {

        if (actividades[i].estado == EJECUTANDO &&
            actividades[i].pid > 0) {

            kill(actividades[i].pid, SIGTERM);
        }

        if (actividades[i].estado == PENDIENTE) {
            actividades[i].estado = ABORTADA;
        }
    }
}

/* -------------------------------------------------- */
/* CREACION DE PROCESOS Y PIPES                       */
/* -------------------------------------------------- */

static int iniciar_actividad(int indice)
{
    Actividad *a = &actividades[indice];

    if (pipe(a->pipe_fd) == -1) {
        perror("pipe");
        return -1;
    }

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");

        close(a->pipe_fd[0]);
        close(a->pipe_fd[1]);

        a->pipe_fd[0] = -1;
        a->pipe_fd[1] = -1;

        return -1;
    }

    if (pid == 0) {

        close(a->pipe_fd[0]);

        /*
         * Mecanismo opcional para probar aislamiento de errores.
         *
         * Ejemplo:
         * FALLAR_ID=1 ./planificador plan.txt 3
         */

        const char *fallar_id = getenv("FALLAR_ID");

        if (fallar_id != NULL &&
            strcmp(fallar_id, a->id) == 0) {

            printf(
                "[FALLO SIMULADO] PID=%ld | %s\n",
                (long)getpid(),
                a->nombre
            );

            fflush(stdout);

            close(a->pipe_fd[1]);

            _exit(EXIT_FAILURE);
        }

        printf(
            "[INICIO] PID=%ld | %s | %d ms\n",
            (long)getpid(),
            a->nombre,
            a->tiempo_ms
        );

        fflush(stdout);

        struct timespec ts;

        ts.tv_sec = a->tiempo_ms / 1000;
        ts.tv_nsec =
            (long)(a->tiempo_ms % 1000) * 1000000L;

        while (nanosleep(&ts, &ts) == -1) {

            if (errno != EINTR) {
                close(a->pipe_fd[1]);
                _exit(EXIT_FAILURE);
            }
        }

        char mensaje[MAX_MENSAJE];

        int largo = snprintf(
            mensaje,
            sizeof(mensaje),
            "Actividad %s (%s) finalizada",
            a->id,
            a->nombre
        );

        if (largo < 0) {
            close(a->pipe_fd[1]);
            _exit(EXIT_FAILURE);
        }

        size_t cantidad = strlen(mensaje) + 1;

        ssize_t escritos =
            write(
                a->pipe_fd[1],
                mensaje,
                cantidad
            );

        if (escritos < 0) {
            close(a->pipe_fd[1]);
            _exit(EXIT_FAILURE);
        }

        close(a->pipe_fd[1]);

        printf(
            "[FIN] PID=%ld | %s\n",
            (long)getpid(),
            a->nombre
        );

        fflush(stdout);

        _exit(EXIT_SUCCESS);
    }

    close(a->pipe_fd[1]);
    a->pipe_fd[1] = -1;

    a->pid = pid;
    a->estado = EJECUTANDO;

    return 0;
}

/* -------------------------------------------------- */
/* FINALIZACION DE PROCESOS                           */
/* -------------------------------------------------- */

static void procesar_hijo_terminado(
    pid_t terminado,
    int status
)
{
    int indice = buscar_por_pid(terminado);

    if (indice < 0) {
        return;
    }

    Actividad *a = &actividades[indice];

    char mensaje[MAX_MENSAJE];

    ssize_t bytes =
        read(
            a->pipe_fd[0],
            mensaje,
            sizeof(mensaje) - 1
        );

    if (bytes > 0) {

        mensaje[bytes] = '\0';

        printf(
            "[PIPE] %s\n",
            mensaje
        );
    }

    if (a->pipe_fd[0] >= 0) {
        close(a->pipe_fd[0]);
        a->pipe_fd[0] = -1;
    }

    if (WIFEXITED(status) &&
        WEXITSTATUS(status) == EXIT_SUCCESS) {

        a->estado = TERMINADA;

    } else {

        a->estado = FALLIDA;

        printf(
            "[FALLO] %s\n",
            a->nombre
        );
    }

    a->pid = -1;
}

/* -------------------------------------------------- */
/* ESTADO GENERAL                                     */
/* -------------------------------------------------- */

static int todo_finalizado(void)
{
    for (int i = 0; i < num_actividades; i++) {

        if (actividades[i].estado == PENDIENTE ||
            actividades[i].estado == EJECUTANDO) {

            return 0;
        }
    }

    return 1;
}

/* -------------------------------------------------- */
/* PLANIFICADOR                                       */
/* -------------------------------------------------- */

static void ejecutar_planificador(int K)
{
    int ejecutando = 0;

    while (!todo_finalizado()) {

        if (sigint_recibido) {

            abortar_todo();

            int status;
            pid_t pid;

            while ((pid = waitpid(-1, &status, 0)) > 0) {

                int indice = buscar_por_pid(pid);

                if (indice >= 0) {
                    actividades[indice].estado = ABORTADA;

                    if (actividades[indice].pipe_fd[0] >= 0) {
                        close(actividades[indice].pipe_fd[0]);
                        actividades[indice].pipe_fd[0] = -1;
                    }

                    actividades[indice].pid = -1;
                }
            }

            return;
        }

        /*
         * Propaga fallos por todas las ramas dependientes.
         */
        while (actualizar_abortadas() > 0) {
        }

        /*
         * Crear solo hasta K procesos concurrentes.
         */
        for (int i = 0;
             i < num_actividades && ejecutando < K;
             i++) {

            if (actividad_lista(i)) {

                if (iniciar_actividad(i) == 0) {
                    ejecutando++;
                } else {
                    actividades[i].estado = FALLIDA;
                }
            }
        }

        /*
         * Si existen procesos activos, esperamos de forma
         * bloqueante. Esto evita busy-waiting.
         */
        if (ejecutando > 0) {

            int status;

            pid_t terminado =
                waitpid(-1, &status, 0);

            if (terminado > 0) {

                procesar_hijo_terminado(
                    terminado,
                    status
                );

                ejecutando--;

            } else if (terminado == -1 &&
                       errno == EINTR) {

                continue;

            } else if (terminado == -1) {

                perror("waitpid");
                break;
            }

            continue;
        }

        /*
         * Si no hay procesos ejecutándose y quedan
         * actividades pendientes, ninguna pudo comenzar.
         * Esto indica un ciclo o bloqueo del DAG.
         */

        int hay_pendientes = 0;

        for (int i = 0;
             i < num_actividades;
             i++) {

            if (actividades[i].estado == PENDIENTE) {
                hay_pendientes = 1;
                break;
            }
        }

        if (hay_pendientes) {

            fprintf(
                stderr,
                "Error: no se puede continuar. "
                "Posible ciclo o dependencias bloqueadas.\n"
            );

            break;
        }
    }
}

/* -------------------------------------------------- */
/* RESUMEN                                            */
/* -------------------------------------------------- */

static const char *nombre_estado(int estado)
{
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

static void imprimir_resumen(void)
{
    printf("\n========== RESUMEN ==========\n");

    for (int i = 0; i < num_actividades; i++) {

        printf(
            "%s - %-20s : %s\n",
            actividades[i].id,
            actividades[i].nombre,
            nombre_estado(actividades[i].estado)
        );
    }
}

/* -------------------------------------------------- */
/* MAIN                                               */
/* -------------------------------------------------- */

int main(int argc, char *argv[])
{
    if (argc != 3) {

        fprintf(
            stderr,
            "Uso: %s plan.txt K\n",
            argv[0]
        );

        return EXIT_FAILURE;
    }

    char *fin_k = NULL;

    long valor_k =
        strtol(argv[2], &fin_k, 10);

    if (*argv[2] == '\0' ||
        *fin_k != '\0' ||
        valor_k <= 0 ||
        valor_k > MAX_ACTIVIDADES) {

        fprintf(
            stderr,
            "Error: K debe ser un entero entre 1 y %d.\n",
            MAX_ACTIVIDADES
        );

        return EXIT_FAILURE;
    }

    int K = (int)valor_k;

    srand((unsigned int)time(NULL));

    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = manejar_sigint;

    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    leer_plan(argv[1]);

    validar_dependencias();

    printf(
        "Plan cargado: %d actividades\n",
        num_actividades
    );

    printf(
        "Concurrencia maxima K = %d\n\n",
        K
    );

    ejecutar_planificador(K);

    imprimir_resumen();

    return EXIT_SUCCESS;
}