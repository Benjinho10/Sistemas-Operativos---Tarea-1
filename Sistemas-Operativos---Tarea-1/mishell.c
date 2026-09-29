// Bibliotecas estándar de C
#include <stdio.h> 
#include <stdlib.h>

// Llamadas al sistema POSIX y control de procesos
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>

// Necesario para sigaction y SIGCHLD
#include <signal.h>

// Tamaños máximos establecidos para buffer de entrada, argumentos y cantidad de jobs
#define MAX 1024 
#define MAX_ARGUMENTOS 64
#define MAX_JOBS 64

// Estructura para registrar los procesos en background
typedef struct {
    int id;               // Número de job
    pid_t pid;           
    char cmd[MAX];       
    int terminado;        // 0 si sigue corriendo, 1 si ya terminó
} Job;

Job jobs[MAX_JOBS];
int num_jobs = 0;

// Recolector asíncrono con SIGCHLD y waitpid(-1, ..., WNOHANG)
void manejador_sigchld(int sig) {
    (void)sig; // Evita advertencia de parámetro no usado
    int status;
    pid_t pid;

    // Recolecta todos los hijos terminados en un bucle sin bloquear al padre
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < num_jobs; i++) {
            if (jobs[i].pid == pid) {
                jobs[i].terminado = 1; // Marcamos el job como listo para notificarlo
                break;
            }
        }
    }
}

// Revisa la lista e imprime al usuario los trabajos finalizados antes de mostrar el nuevo prompt
void notificar_jobs_terminados(void) {
    for (int i = 0; i < num_jobs; i++) {
        if (jobs[i].terminado) {
            printf("[%d]+  Done                    %s\n", jobs[i].id, jobs[i].cmd);
            fflush(stdout);
            
            // Eliminamos el job del arreglo desplazando los posteriores
            for (int j = i; j < num_jobs - 1; j++) {
                jobs[j] = jobs[j + 1];
            }
            num_jobs--;
            i--; // Reevaluar la posición actual
        }
    }
}

// Lógica generalizada a partir del esqueleto de tubería de diapositiva de clases
void ejecutar_pipeline(char ***comandos, int n_comandos, int background, const char *cmd_line) {
    int prev_fd = -1; 
    pid_t pids[n_comandos]; 

    for (int i = 0; i < n_comandos; i++) {
        int pipefd[2];

        // Creación del pipe si no es el último comando
        if (i < n_comandos - 1) {
            if (pipe(pipefd) < 0) {
                perror("pipe");
                return;
            }
        }

        // Creación del proceso hijo con fork()
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return;
        }

        if (pid == 0) {
            // Configurar señales en el hijo del pipeline
            struct sigaction sa_pipe;
            sigemptyset(&sa_pipe.sa_mask);
            sa_pipe.sa_flags = 0;
            sa_pipe.sa_handler = background ? SIG_IGN : SIG_DFL;
            sigaction(SIGINT, &sa_pipe, NULL);
            sigaction(SIGQUIT, &sa_pipe, NULL);

            // Extremo de lectura
            if (prev_fd != -1) {
                dup2(prev_fd, STDIN_FILENO);
                close(prev_fd);
            }

            // Extremo de escritura
            if (i < n_comandos - 1) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
            }

            execvp(comandos[i][0], comandos[i]);
            perror(comandos[i][0]);
            _exit(127);
        }

        // Proceso padre (shell): recolecta descriptores y guarda PIDs
        pids[i] = pid;

        if (prev_fd != -1) {
            close(prev_fd);
        }

        if (i < n_comandos - 1) {
            close(pipefd[1]);       // Cierre en el padre para enviar EOF al lector
            prev_fd = pipefd[0];
        }
    }

    if (background) {
        int job_id = num_jobs + 1;
        jobs[num_jobs].id = job_id;
        jobs[num_jobs].pid = pids[n_comandos - 1];
        strncpy(jobs[num_jobs].cmd, cmd_line, MAX - 1);
        jobs[num_jobs].terminado = 0;
        num_jobs++;

        printf("[%d] %d\n", job_id, pids[n_comandos - 1]);
        fflush(stdout);
    } else {
        // Padre sincronizando a los hijos con waitpid()
        for (int i = 0; i < n_comandos; i++) {
            int status;
            waitpid(pids[i], &status, 0);
        }
    }
}

// Comando interno jobs
void ejecutar_jobs(void) {
    for (int i = 0; i < num_jobs; i++) {
        if (!jobs[i].terminado) {
            printf("[%d]  Ejecutando                    %s\n", jobs[i].id, jobs[i].cmd);
        }
    }
}

// Estructura para almacenar métricas previas y calcular %CPU
typedef struct {
    unsigned long long prev_ticks;
} MetricaCPU;

MetricaCPU metricas[MAX_JOBS];

// Obtiene estado y ticks acumulados desde /proc/[pid]/stat
int obtener_stat_proc(pid_t pid, char *estado_str, unsigned long long *total_ticks) {
    char ruta[64];
    snprintf(ruta, sizeof(ruta), "/proc/%d/stat", pid);
    FILE *f = fopen(ruta, "r");
    if (!f) return 0; // El proceso terminó entre lecturas

    char buffer[1024];
    if (!fgets(buffer, sizeof(buffer), f)) {
        fclose(f);
        return 0;
    }
    fclose(f);

    // El nombre del ejecutable puede tener espacios y va entre paréntesis, se busca el último ')'
    char *cierre_parentesis = strrchr(buffer, ')');
    if (!cierre_parentesis) return 0;

    char state_char;
    unsigned long utime = 0, stime = 0;
    
    // Leemos a partir del paréntesis de cierre: estado es campo 3; utime y stime son campos 14 y 15
    sscanf(cierre_parentesis + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", 
           &state_char, &utime, &stime);

    *total_ticks = (unsigned long long)(utime + stime);

    switch (state_char) {
        case 'R': strcpy(estado_str, "ejecutando"); break;
        case 'S': strcpy(estado_str, "durmiendo"); break;
        case 'Z': strcpy(estado_str, "zombie"); break;
        case 'T': strcpy(estado_str, "detenido"); break;
        default:  strcpy(estado_str, "desconocido"); break;
    }
    return 1;
}

// Obtiene VmRSS en KB desde /proc/[pid]/status
long obtener_rss_proc(pid_t pid) {
    char ruta[64];
    snprintf(ruta, sizeof(ruta), "/proc/%d/status", pid);
    FILE *f = fopen(ruta, "r");
    if (!f) return 0;

    char linea[256];
    long rss_kb = 0;
    while (fgets(linea, sizeof(linea), f)) {
        if (strncmp(linea, "VmRSS:", 6) == 0) {
            sscanf(linea + 6, "%ld", &rss_kb);
            break;
        }
    }
    fclose(f);
    return rss_kb;
}

volatile sig_atomic_t pmon_tick = 0;
volatile sig_atomic_t pmon_salir = 0;

void manejador_sigalrm(int sig) {
    (void)sig;
    pmon_tick = 1;
}

void manejador_sigint_pmon(int sig) {
    (void)sig;
    pmon_salir = 1;
}

void ejecutar_pmon(int intervalo) {
    // Configurar manejador temporal de SIGALRM
    struct sigaction sa_alrm, sa_alrm_old;
    sa_alrm.sa_handler = manejador_sigalrm;
    sigemptyset(&sa_alrm.sa_mask);
    sa_alrm.sa_flags = 0; // Sin SA_RESTART para que pause() despierte
    sigaction(SIGALRM, &sa_alrm, &sa_alrm_old);

    // Configurar manejador temporal de SIGINT (Ctrl+C sale de pmon sin cerrar la shell)
    struct sigaction sa_int, sa_int_old;
    sa_int.sa_handler = manejador_sigint_pmon;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = 0;
    sigaction(SIGINT, &sa_int, &sa_int_old);

    long clk_tck = sysconf(_SC_CLK_TCK);
    pmon_salir = 0;
    pmon_tick = 1; // Forzar primer dibujado inmediato

    // Inicializar métricas previas
    for (int i = 0; i < num_jobs; i++) {
        char est[32];
        unsigned long long t;
        if (obtener_stat_proc(jobs[i].pid, est, &t)) {
            metricas[i].prev_ticks = t;
        } else {
            metricas[i].prev_ticks = 0;
        }
    }

    while (!pmon_salir) {
        if (pmon_tick) {
            pmon_tick = 0;

            // Limpiar terminal o encabezado
            printf("\033[H\033[J"); // Secuencia ANSI para limpiar pantalla
            printf("%-8s %-20s %-12s %-12s %-10s\n", "PID", "COMANDO", "ESTADO", "%CPU (aprox)", "RSS (KB)");

            for (int i = 0; i < num_jobs; i++) {
                if (jobs[i].terminado) continue;

                char estado[32];
                unsigned long long ticks_actuales = 0;
                if (!obtener_stat_proc(jobs[i].pid, estado, &ticks_actuales)) {
                    continue; // El proceso terminó
                }
                long rss = obtener_rss_proc(jobs[i].pid);

                double cpu_pct = 0.0;
                if (ticks_actuales >= metricas[i].prev_ticks) {
                    unsigned long long delta_ticks = ticks_actuales - metricas[i].prev_ticks;
                    double tiempo_cpu_sec = (double)delta_ticks / (double)clk_tck;
                    cpu_pct = (tiempo_cpu_sec / (double)intervalo) * 100.0;
                }
                metricas[i].prev_ticks = ticks_actuales;

                printf("%-8d %-20s %-12s %-12.1f %-10ld\n", 
                       jobs[i].pid, jobs[i].cmd, estado, cpu_pct, rss);
            }
            fflush(stdout);

            if (!pmon_salir) {
                alarm(intervalo);
            }
        }

        pause(); // Espera pasivamente la llegada de SIGALRM o SIGINT
    }

    // Cancelar cualquier alarma pendiente
    alarm(0);

    // Restaurar manejadores previos de la shell
    sigaction(SIGALRM, &sa_alrm_old, NULL);
    sigaction(SIGINT, &sa_int_old, NULL);
    printf("\n");
}

int main(void) {
    // Configuración del manejador de SIGCHLD
    struct sigaction sa;
    sa.sa_handler = manejador_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    // Ignorar SIGINT (Ctrl+C) y SIGQUIT (Ctrl+\) en el proceso shell
    struct sigaction sa_ignore;
    sa_ignore.sa_handler = SIG_IGN;
    sigemptyset(&sa_ignore.sa_mask);
    sa_ignore.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa_ignore, NULL);
    sigaction(SIGQUIT, &sa_ignore, NULL);

    char cwd[MAX];
    char line[MAX];
    char line_copia[MAX];
    char *args[MAX_ARGUMENTOS];

    // Ciclo principal de la shell ("mostrar prompt -> leer -> parsear -> ejecutar")
    while (1) {
        // Notifica jobs que hayan terminado antes de dibujar el prompt
        notificar_jobs_terminados();

        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            printf("%s$ ", cwd);
        } else {
            printf("msh> ");
        }
        fflush(stdout); // Vaciar el buffer del prompt

        // Leer línea con fgets() y detectar fin de archivo
        if (fgets(line, sizeof(line), stdin) == NULL) {
            printf("\n");
            break;
        }

        // Quitar salto de línea si viene incluido
        line[strcspn(line, "\n")] = '\0';

        // Respaldamos la línea completa antes de que strtok() la corte (para el mensaje de los jobs)
        strncpy(line_copia, line, MAX - 1);
        line_copia[MAX - 1] = '\0';

        // Separar argumentos usando strtok()
        int argc = 0;
        char *tok = strtok(line, " \t");
        while (tok != NULL && argc < MAX_ARGUMENTOS - 1) {
            args[argc++] = tok;
            tok = strtok(NULL, " \t");
        }
        args[argc] = NULL;

        // Si no se ingresaron comandos, continúa el ciclo
        if (argc == 0) {
            continue;
        }

        // Detectar operador background
        int background = 0;
        if (argc > 0 && strcmp(args[argc - 1], "&") == 0) {
            background = 1;
            args[argc - 1] = NULL; // Eliminamos el '&' para que no rompa execvp()
            argc--;
        }

        if (argc == 0) {
            continue;
        }

        //Implementación de comandos internos
        if (strcmp(args[0], "exit") == 0) {
            int codigo = (argc > 1) ? atoi(args[1]) : 0;
            exit(codigo);
        }

        if (strcmp(args[0], "cd") == 0) {
            const char *dir = (argc > 1) ? args[1] : getenv("HOME");
            if (dir == NULL || chdir(dir) < 0) {
                perror("cd");
            }
            continue;
        }

        if (strcmp(args[0], "jobs") == 0) {
            ejecutar_jobs();
            continue;
        }

        if (strcmp(args[0], "pmon") == 0) {
            int seg = (argc > 1) ? atoi(args[1]) : 2;
            if (seg <= 0) seg = 2;
            ejecutar_pmon(seg);
            continue;
        }

        // Detección de pipes
        int hay_pipe = 0;
        for (int i = 0; args[i] != NULL; i++) {
            if (strcmp(args[i], "|") == 0) {
                hay_pipe = 1;
                break;
            }
        }

        if (hay_pipe) {
            char **comandos[MAX_ARGUMENTOS];
            int n_comandos = 0;

            comandos[n_comandos++] = &args[0];

            for (int i = 0; i < argc; i++) {
                if (args[i] != NULL && strcmp(args[i], "|") == 0) {
                    args[i] = NULL;
                    if (args[i + 1] != NULL) {
                        comandos[n_comandos++] = &args[i + 1];
                    }
                }
            }

            if (n_comandos < 2 || comandos[n_comandos - 1][0] == NULL) {
                fprintf(stderr, "Error sintáctico: comando incompleto en la tubería\n");
            } else {
                ejecutar_pipeline(comandos, n_comandos, background, line_copia);
            }
            continue;
        }

        // patrón fork() + execvp() + waitpid()
    
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
        } 
        else if (pid == 0) {
            // Manejo de señales en el hijo
            struct sigaction sa_default;
            sigemptyset(&sa_default.sa_mask);
            sa_default.sa_flags = 0;
            sa_default.sa_handler = background ? SIG_IGN : SIG_DFL;
            sigaction(SIGINT, &sa_default, NULL);
            sigaction(SIGQUIT, &sa_default, NULL);

            // Redirecciones con open(), dup2() y close()
            for (int i = 0; args[i] != NULL; i++) {
                if (strcmp(args[i], "<") == 0) {
                    char *archivo = args[i + 1];
                    if (archivo == NULL) {
                        fprintf(stderr, "Error sintáctico: falta archivo tras '<'\n");
                        _exit(1);
                    }
                    int fd = open(archivo, O_RDONLY);
                    if (fd < 0) {
                        perror("open");
                        _exit(1);
                    }
                    dup2(fd, STDIN_FILENO);
                    close(fd);
                    args[i] = NULL;
                }
                else if (strcmp(args[i], ">") == 0) {
                    char *archivo = args[i + 1];
                    if (archivo == NULL) {
                        fprintf(stderr, "Error sintáctico: falta archivo tras '>'\n");
                        _exit(1);
                    }
                    int fd = open(archivo, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (fd < 0) {
                        perror("open");
                        _exit(1);
                    }
                    if (dup2(fd, STDOUT_FILENO) < 0) {
                        perror("dup2");
                        _exit(1);
                    }
                    close(fd);
                    args[i] = NULL;
                }
                else if (strcmp(args[i], ">>") == 0) {
                    char *archivo = args[i + 1];
                    if (archivo == NULL) {
                        fprintf(stderr, "Error sintáctico: falta archivo tras '>>'\n");
                        _exit(1);
                    }
                    int fd = open(archivo, O_WRONLY | O_CREAT | O_APPEND, 0644);
                    if (fd < 0) {
                        perror("open");
                        _exit(1);
                    }
                    dup2(fd, STDOUT_FILENO);
                    close(fd);
                    args[i] = NULL;
                }
            }

            execvp(args[0], args);
            perror(args[0]);
            _exit(127);
        } 
        else {
            // Gestión padre: foreground vs background
            if (background) {
                int job_id = num_jobs + 1;
                jobs[num_jobs].id = job_id;
                jobs[num_jobs].pid = pid;
                strncpy(jobs[num_jobs].cmd, line_copia, MAX - 1);
                jobs[num_jobs].terminado = 0;
                num_jobs++;

                printf("[%d] %d\n", job_id, pid);
                fflush(stdout);
            } else {
                int status;
                if (waitpid(pid, &status, 0) < 0) {
                    perror("waitpid");
                }
            }
        }
    }

    return 0;
}