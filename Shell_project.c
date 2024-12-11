/**
UNIX Shell Project

Sistemas Operativos
Grados I. Informatica, Computadores & Software
Dept. Arquitectura de Computadores - UMA
**/

#include <string.h>   // Para trabajar con cadenas de texto
#include <errno.h>    // Para manejar errores del sistema
#include "job_control.h" // Biblioteca personalizada para control de trabajos
#include "parse_redir.h" // Biblioteca personalizada para parsear redirecciones

// Definiciones para colores en la terminal
#define ROJO "\x1b[31;1;1m"
#define NEGRO "\x1b[0m"
#define VERDE "\x1b[32;1;1m"
#define AZUL "\x1b[34;1;1m"
#define CIAN "\x1b[36;1;1m"
#define MARRON "\x1b[33;1;1m"
#define PURPURA "\x1b[35;1;1m"
#define RESET "\033[0m"

#define MAX_LINE 256 /* Longitud máxima de línea permitida por comando */

// Lista global para almacenar los trabajos
job *job_list;

// Manejador de la señal SIGCHLD, que se activa cuando un proceso hijo cambia de estado
void sigchld_handler(int sig) {
    int status_c;              // Estado del proceso hijo
    pid_t pid_c;               // PID del proceso hijo
    int info_c;                // Información adicional sobre el estado del proceso
    enum status status_res_c;  // Resultado del análisis del estado

    // Recorremos todos los procesos hijos que han cambiado de estado
    while ((pid_c = waitpid(-1, &status_c, WNOHANG | WUNTRACED | 8)) > 0) {
        // Analizar el estado del proceso hijo
        status_res_c = analyze_status(status_c, &info_c);

        // Imprimir información sobre el proceso finalizado
        if (status_res_c != CONTINUED){ // Si el proceso ha acabado o ha sido señalizado
            printf(VERDE "\nBackground process %d finished: %s\n" RESET, pid_c, status_strings[status_res_c]);
        fflush(stdout); // Forzar la salida a la terminal inmediatamente
        // Reimprimir el prompt para que el usuario pueda continuar escribiendo
        printf(AZUL "COMMAND->" RESET);
        fflush(stdout);
        }

        // Si el proceso ha terminado o ha sido señalizado, actualizar la lista de trabajos
        if (status_res_c == EXITED || status_res_c == SIGNALED) {
            job *finished_job = get_item_bypid(job_list, pid_c);
            if (finished_job) {
                delete_job(job_list, finished_job); // Eliminar el trabajo de la lista
            }
        } else if (status_res_c == SUSPENDED) { // Si el proceso ha sido detenido
            job *suspended_job = get_item_bypid(job_list, pid_c);
            if (suspended_job) {
                suspended_job->state = STOPPED; // Actualizar el estado del trabajo
            }
        } else if (status_res_c == CONTINUED) { // Si un proceso suspendido recibe la señal para continuar
			job *continued_job = get_item_bypid(job_list, pid_c);
			if (continued_job && continued_job->state == STOPPED) {
				continued_job->state = BACKGROUND; // Cambiar estado a background
			}
		}

    }
}

int main(void)
{
    char inputBuffer[MAX_LINE]; /* Buffer para almacenar el comando introducido */
    int background;             /* Indica si un comando debe ejecutarse en segundo plano (&) */
    char *args[MAX_LINE/2];     /* Lista de argumentos del comando */

    // Variables para el control de procesos
    int pid_fork, pid_wait; /* PIDs para el proceso creado y esperado */
    int status;             /* Estado devuelto por wait */
    enum status status_res; /* Resultado del análisis del estado */
    int info;               /* Información procesada por analyze_status */
    int pid_shell = getpid(); /* PID del proceso principal (shell) */
    
    job *njob; /* Variable para almacenar un nuevo trabajo */

    // Inicializar la lista de trabajos
    job_list = new_list("Lista de trabajos");

    // Registrar el manejador para la señal SIGCHLD
    signal(SIGCHLD, sigchld_handler);

    // Ignorar señales en el shell principal
    ignore_terminal_signals();

    printf(PURPURA "Welcome to the shell!\n" RESET);

    while (1) {  /* Bucle principal del shell */
        printf(AZUL "COMMAND->" RESET);
        fflush(stdout); // Asegurar que el prompt se imprime inmediatamente

        // Obtener el comando del usuario
        get_command(inputBuffer, MAX_LINE, args, &background);

        if (args[0] == NULL) continue; /* Ignorar comandos vacíos */

        // Parseamos las redirecciones de entrada y salida
        char *file_in, *file_out;
        parse_redirections(args, &file_in, &file_out);

        if (args[0] == NULL) {
            fprintf(stderr, ROJO "syntax error in redirection\n" RESET);
            continue; // ignoramos este comando y volvemos al bucle principal
        }

        /* =========================    COMANDOS INTERNOS    ========================= */

        // Comando interno: cambiar de directorio (cd)
        if (strcmp(args[0], "cd") == 0) {
            if (args[1] == NULL) {
                chdir(getenv("HOME")); // Cambiar al directorio HOME
            } else {
                if (chdir(args[1]) == -1) { // Cambiar al directorio especificado
                    printf(ROJO "Error: Directorio no encontrado\n" RESET);
                }
            }
            continue; // Volver al inicio del bucle principal
        }

        // Comando interno: mostrar la lista de trabajos (jobs)
		if (strcmp(args[0], "jobs") == 0) {
			block_SIGCHLD(); // Bloqueamos las señales SIGCHLD para evitar condiciones de carrera
			if (empty_list(job_list)) { // Si la lista esta vacia, imprimimos que no hay tareas
				printf(ROJO "No hay tareas en segundo plano o suspendidas.\n" RESET);
			} else {
				print_job_list(job_list); // Imprimimos la lista de tareas
			}
			unblock_SIGCHLD(); // Desbloqueamos las señales SIGCHLD
			continue; // Volver al inicio del bucle principal
        }

        // Comando interno: poner en primer plano un trabajo (fg)
        if (strcmp(args[0], "fg") == 0) {
            int n = 1; 
            // Comprobamos si se ha pasado un argumento para seleccionar la posición
            // del trabajo en la lista. Si no, usamos n=1 (el primer trabajo).
            if (args[1] != NULL) {
                n = atoi(args[1]);
                if (n <= 0) {
                    printf(ROJO "fg: Argumento inválido\n" RESET);
                    continue; // Argumento inválido, volver al bucle principal
                }
            }

            // Bloqueamos SIGCHLD antes de acceder a la lista
            block_SIGCHLD();
            // Obtenemos el trabajo por su posición en la lista
            job * fg_job = get_item_bypos(job_list, n);
            if (fg_job == NULL) {
                printf(ROJO "fg: no existe un trabajo en esa posición\n" RESET);
                // Desbloqueamos SIGCHLD si no encontramos el trabajo
                unblock_SIGCHLD();
                continue; // Volver al bucle principal
            }

            // Cambiamos el estado del trabajo a FOREGROUND
            fg_job->state = FOREGROUND;
            // Ya no necesitamos acceso exclusivo a la lista
            unblock_SIGCHLD();

            // Cedemos el terminal al grupo de procesos del trabajo
            set_terminal(fg_job->pgid);
            // Enviamos señal SIGCONT por si el trabajo estaba detenido
            killpg(fg_job->pgid, SIGCONT);

            int status;
            int info;
            enum status status_res;

            // Esperamos al proceso en primer plano (puede finalizar o suspenderse)
            pid_t pid_wait = waitpid(fg_job->pgid, &status, WUNTRACED);
            // Analizamos el estado en que terminó o cambió el proceso
            status_res = analyze_status(status, &info);

            // Si el proceso vuelve a suspenderse, lo marcamos como STOPPED
            if (status_res == SUSPENDED) {
                block_SIGCHLD();
                fg_job->state = STOPPED;
                unblock_SIGCHLD();
                printf(VERDE "Proceso %d suspendido de nuevo.\n" RESET, fg_job->pgid);
            } else if (status_res == EXITED || status_res == SIGNALED) {
                // Si el proceso ha terminado o ha sido señalizado, lo eliminamos de la lista
                block_SIGCHLD();
                printf(VERDE "Foreground pid: %d, Command: %s, Status: %s, Info: %d\n" RESET, 
                    pid_wait, fg_job->command, status_strings[status_res], info);
                delete_job(job_list, fg_job);
                unblock_SIGCHLD();
            }

            // Devolvemos el terminal al shell
            set_terminal(getpid());

            continue; // Volver al inicio del bucle principal
        }

        // Comando interno: poner en segundo plano un trabajo suspendido (bg)
        if (strcmp(args[0], "bg") == 0) {
            int n = 1; 
            // Si el usuario especifica un número, lo convertimos a entero.
            // Si no se especifica, n=1 se aplicará al primer trabajo de la lista.
            if (args[1] != NULL) {
                n = atoi(args[1]);
                if (n <= 0) {
                    printf(ROJO "bg: Argumento inválido\n" RESET);
                    continue; // Argumento inválido, volvemos al bucle principal.
                }
            }

            // Bloqueamos SIGCHLD antes de acceder a la lista de trabajos.
            block_SIGCHLD();
            job *bg_job = get_item_bypos(job_list, n);
            if (bg_job == NULL) {
                // Si no encontramos un trabajo en esa posición, informamos y continuamos.
                printf(ROJO "bg: no existe un trabajo en esa posición\n" RESET);
                unblock_SIGCHLD();
                continue;
            }

            // Verificamos que el trabajo esté suspendido (STOPPED).
            if (bg_job->state != STOPPED) {
                // Si no está suspendido, no podemos ponerlo en bg.
                printf(ROJO "bg: el trabajo seleccionado no está suspendido\n" RESET);
                unblock_SIGCHLD();
                continue;
            }

            // Cambiamos el estado a BACKGROUND
            bg_job->state = BACKGROUND;
            // Ya no necesitamos acceso exclusivo a la lista
            unblock_SIGCHLD();

            // Enviamos SIGCONT al grupo de procesos del trabajo para reanudarlo en segundo plano.
            killpg(bg_job->pgid, SIGCONT);

            // Indicamos al usuario que el trabajo se ha reanudado en segundo plano.
            printf(VERDE "Tarea %d reanudada en segundo plano: PID: %d, Command: %s\n" RESET, 
                   n, bg_job->pgid, bg_job->command);

            continue; // Volver al bucle principal
        }

		/* =========================    COMANDOS INTERNOS    ========================= */

        // Crear un nuevo proceso con fork
        pid_fork = fork();

        switch (pid_fork) {
            case -1: // Error al crear el proceso
                perror(ROJO "Error: fork() failed\n" RESET);
                exit(-1);

            case 0: /* Proceso hijo */
                restore_terminal_signals(); /* Restaurar señales por defecto */
                new_process_group(pid_fork); /* Crear un nuevo grupo de procesos */

                // Redirecciones de entrada y salida
                        
                if (file_in != NULL) { // Redirección de entrada si file_in no es NULL
                    int fd_in = open(file_in, O_RDONLY);
                    if (fd_in < 0) {
                        perror(ROJO "Error abriendo fichero de entrada" RESET);
                        exit(1);
                    }
                    if (dup2(fd_in, STDIN_FILENO) < 0) {
                        perror(ROJO "Error en dup2 para entrada" RESET);
                        close(fd_in);
                        exit(1);
                    }
                    close(fd_in); // ya no necesitamos el descriptor original
                }

                if (file_out != NULL) { // Redirección de salida si file_out no es NULL
                    // Abrir en escritura, crear si no existe, truncar si existe
                    int fd_out = open(file_out, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                    if (fd_out < 0) {
                        perror(ROJO "Error abriendo fichero de salida" RESET);
                        exit(1);
                    }
                    if (dup2(fd_out, STDOUT_FILENO) < 0) {
                        perror(ROJO "Error en dup2 para salida" RESET);
                        close(fd_out);
                        exit(1);
                    }
                    close(fd_out); // ya no necesitamos el descriptor original
                }

                execvp(args[0], args); /* Intentar ejecutar el comando */

                // Manejo de errores si execvp falla
                switch (errno) {
                    case ENOENT: /* Archivo no encontrado */
                        fprintf(stderr, ROJO "error: command not found: %s\n" RESET, args[0]);
                        exit(127);
                    case EACCES: /* Permisos insuficientes */
                        fprintf(stderr, ROJO "error: permission denied: %s\n" RESET, args[0]);
                        exit(126);
                    case ENOEXEC: /* No es un ejecutable válido */
                        fprintf(stderr, ROJO "error: not an executable: %s\n" RESET, args[0]);
                        exit(126);
                    default: /* Otros errores */
                        fprintf(stderr, ROJO "error: execvp failed (%s): %s\n" RESET, strerror(errno), args[0]);
                        exit(EXIT_FAILURE);
                }

            default: /* Proceso padre */
                if (background == 0) { /* Comando en primer plano */
                    set_terminal(pid_fork); /* Asignar terminal al hijo */
                    pid_wait = waitpid(pid_fork, &status, WUNTRACED);
                    set_terminal(pid_shell); /* Devolver terminal al shell */
                    status_res = analyze_status(status, &info);

					// Comprobamos el estado del hijo
                    switch (status_res) {
                        case SUSPENDED: /* Si ha sido suspendido lo añadimos a jobs */
                            njob = new_job(pid_fork, args[0], STOPPED);

							block_SIGCHLD();
                            add_job(job_list, njob); /* Bloqueamos y desbloqueamos la lista para evitar condiciones de carrera */
							unblock_SIGCHLD();
						
						default: /* Si no ha sido suspendido ha acabado */
                            printf(VERDE "Foreground pid: %d, Command: %s, Status: %s, Info: %d\n" RESET, 
                                pid_wait, args[0], status_strings[status_res], info);
                            break;
                    }

                } else { /* Comando en segundo plano */
					// Añadir el proceso en segundo plano a la lista de trabajos
					njob = new_job(pid_fork, args[0], BACKGROUND);
					block_SIGCHLD();
                    add_job(job_list, njob); /* Bloqueamos y desbloqueamos la lista para evitar condiciones de carrera */
					unblock_SIGCHLD();

                    printf(VERDE "Background process running -> PID: %d, Command: %s\n" RESET, pid_fork, args[0]);
                }
                break;
        }
    }
}
