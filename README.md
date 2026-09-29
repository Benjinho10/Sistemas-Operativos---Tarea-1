# Sistemas Operativos Tarea 1

Requisitos
* Sistema operativo basado en Linux / WSL (Ubuntu recomendado).
* Compilador `gcc`.
* Utilidad `make`.

Compilación y Ejecución

Se incluye un archivo `Makefile` que automatiza la compilación con las banderas de advertencia y estándares POSIX necesarios (`-Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L`).

### 1. Compilación
Para compilar el código fuente y generar el ejecutable binario `mishell`, ejecuta en la raíz del proyecto:
`make`

Si se desea compilarlo de forma manual sin utilizar make, puedes ejecutar directamente:
`gcc -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L -o mishell mishell.c`

Una vez compilado, inicia la shell con:
`./mishell`

Para salir del intérprete de comandos, escribe:
`exit`

Para eliminar el archivo binario generado y los archivos objeto intermedios:
`make clean`
