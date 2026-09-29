# Tarea 1 - Sistemas Operativos

## Planificador Dieciochero

Este programa simula un plan de actividades usando procesos.

Las actividades se leen desde el archivo `plan.txt`. Cada actividad tiene un ID, nombre, tiempo de ejecución y sus dependencias.

Una actividad solo puede empezar cuando todas sus dependencias hayan terminado.

## Compilar

Para compilar:

```bash
make
```

También se puede usar:

```bash
gcc -Wall -Wextra -std=c17 -lpthread planificador.c -o planificador
```

## Ejecutar

Se ejecuta de esta forma:

```bash
./planificador plan.txt K
```

`K` indica la cantidad máxima de actividades que pueden estar ejecutándose al mismo tiempo.

Por ejemplo:

```bash
./planificador plan.txt 3
```

## Formato de plan.txt

El formato es:

```text
ID : nombre : tiempo_ms : dependencias
```

Ejemplo:

```text
1 : prender_carbon : 500 :
2 : comprar_carne : 1200 :
3 : comprar_pan : 300 :
4 : asar_longaniza : 800 : 1, 2
5 : armar_choripan : 250 : 3, 4
6 : servir_mesa : 100 : 5
```

Si no se indica un tiempo, el programa genera uno aleatorio entre 100 y 5000 ms.

## Cómo funciona

Cada actividad se ejecuta en un proceso hijo creado con `fork()`.

El programa revisa las dependencias antes de iniciar una actividad y controla que no se ejecuten más de `K` actividades al mismo tiempo.

Para esperar a los procesos se usa `waitpid()`, evitando hacer busy-waiting.

También se utilizan pipes para comunicar la finalización de las actividades entre los procesos y el planificador.

No se utilizan threads.

## Errores

Si una actividad falla, el programa no termina completamente. Se abortan las actividades que dependían de la actividad que falló y las otras pueden seguir funcionando.

Para probar un fallo se puede usar:

```bash
FALLAR_ID=1 ./planificador plan.txt 3
```

## Ctrl+C

Si se presiona `Ctrl+C`, el programa recibe `SIGINT` y detiene las actividades que se están ejecutando.

## Archivos

- `planificador.c`: código del programa.
- `plan.txt`: archivo con las actividades.
- `Makefile`: permite compilar más fácilmente.
- `README.md`: explicación básica del programa.