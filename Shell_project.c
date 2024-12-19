/**
UNIX Shell Project

Sistemas Operativos
Grados I. Informatica, Computadores & Software
Dept. Arquitectura de Computadores - UMA
Iván Ballesteros Fernández - 24-25 - 2ºGCIA
**/

#include <string.h>   // Para trabajar con cadenas de texto
#include <errno.h>    // Para manejar errores del sistema
#include "job_control.h" // Biblioteca personalizada para control de trabajos
#include "parse_redir.h" // Biblioteca personalizada para parsear redirecciones
#include "pthread.h" // Biblioteca para trabajar con hilos

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

// Manejador de la señal SIGHUP
void sighup_handler(int sig) {
    FILE *fp;
    fp=fopen("hup.txt","a"); // Abre un fichero en modo 'append'
    fprintf(fp, "SIGHUP recibido.\n"); // Escribe en el fichero
    fclose(fp);
}

/**
 * Manejador de la señal SIGCHLD
 * Se activa cuando un proceso hijo cambia de estado: finaliza, se suspende o continúa.
 * Controla trabajos en segundo plano, incluyendo los trabajos respawnable.
 */
void sigchld_handler(int sig) {
    int pid_c, pid_resp;
    int wstatus, info, grupo;
    job *tarea;

    block_SIGCHLD(); // Bloquear señales SIGCHLD para evitar condiciones de carrera

    while ((pid_c = waitpid(-1, &wstatus, WNOHANG | WUNTRACED | 8)) > 0) {

        // Obtener el estado del proceso
        int status_res = analyze_status(wstatus, &info);
        tarea = get_item_bypid(job_list, pid_c);

        if (tarea == NULL) {
            printf(ROJO "Error: No se encontró la tarea con PID %d\n" RESET, pid_c);
            continue;
        }

        // Imprimir información del proceso
        if (status_res != CONTINUED) {
            printf(VERDE "%s process %d finished: %s\n" RESET,state_strings[tarea->state], pid_c, status_strings[status_res]);
            fflush(stdout);
        }

        if (status_res == SUSPENDED) { 
            // Si el proceso se suspende, actualizar su estado
            tarea->state = STOPPED;

        } else if (status_res == EXITED || status_res == SIGNALED) {
            if (tarea->state == RESPAWNABLE) {
                // Relanzar el proceso respawnable
                pid_resp = fork();
                if (pid_resp == -1) {
                    perror(ROJO "Error: No se pudo relanzar el proceso respawnable\n" RESET);
                } else if (pid_resp == 0) { // Proceso hijo
                    grupo = getpid();
                    setpgid(pid_resp, grupo);
                    restore_terminal_signals(); // Restaurar señales predeterminadas
                    printf(VERDE "Respawnable job relaunched: command: %s, new pid: %d\n" RESET,
                           tarea->command, grupo);
                    printf(AZUL "COMMAND->" RESET);
                    fflush(stdout);
                    execvp(tarea->command, tarea->args);
                    perror(ROJO "execvp failed in respawnable job" RESET);
                    exit(EXIT_FAILURE);
                } else { // Proceso padre
                    setpgid(pid_resp, pid_resp);
                    tarea->pgid = pid_resp; // Actualizar el PGID del trabajo
                }
            } else {
                // Eliminar el trabajo si no es respawnable

                if (delete_job(job_list, tarea) != 1) {
                    printf(ROJO "Error: No se pudo eliminar la tarea con PID %d\n" RESET, pid_c);
                }
            }

        } else if (status_res == CONTINUED) { 
            // Si el proceso continúa, actualizar su estado
            tarea->state = BACKGROUND;
        }
    }
    unblock_SIGCHLD(); // Desbloquear señales SIGCHLD
}


typedef struct ThreadArgs{  // Estructura para pasar argumentos al hilo
	int tiempo;  // esto sera el tiempo de vida del proceso
	int grupo;  // esto sera el pid del proceso
} ThreadArgs;

// Función para el temporizador
void *alarm_thread(void *arg) {
    ThreadArgs *args = (ThreadArgs *) arg;  // Convertimos el argumento a la estructura
    printf(MARRON "Temporizador activado: %d segundos\n" RESET, args->tiempo);  // Informamos al usuario
    fflush(stdout);
    sleep(args->tiempo);  // Esperamos el tiempo especificado
    int error = killpg(args->grupo, SIGKILL);  // Enviamos la señal SIGKILL al grupo de procesos
    if (error == -1) {  // Si hay un error, informamos al usuario
        printf(ROJO "Error al matar el proceso\n" RESET);
    } else {
        printf(MARRON "Proceso %d matado por temporizador\n" RESET, args->grupo);  // Informamos al usuario
    }
    free(args);  // Liberamos la memoria de los argumentos
    return NULL;
}

// Función para el delay-thread
void *delay_thread(void *arg) {
    ThreadArgs *args = (ThreadArgs *) arg;  // Convertimos el argumento a la estructura
    printf(MARRON "Delay activado: %d segundos\n" RESET, args->tiempo);  // Informamos al usuario
    fflush(stdout);
    sleep(args->tiempo);  // Esperamos el tiempo especificado
    free(args);  // Liberamos la memoria de los argumentos
    return NULL;
}


int main(void)
{
    char inputBuffer[MAX_LINE]; /* Buffer para almacenar el comando introducido */
    int background = 0;             /* Indica si un comando debe ejecutarse en segundo plano (&) */
    int respawnable = 0;            /* Indica si un comando debe revivir al morir (+) */
    char *args[MAX_LINE/2];     /* Lista de argumentos del comando */

    // Variables para el control de procesos
    int pid_fork, pid_wait, bg_fork; /* PIDs para el proceso creado y esperado */
    int status;             /* Estado devuelto por wait */
    enum status status_res; /* Resultado del análisis del estado */
    int info;               /* Información procesada por analyze_status */
    int pid_shell = getpid(); /* PID del proceso principal (shell) */

    int thread = 0; // Indica si se ha creado un hilo para el temporizador
    int delay = 0; // Indica si se ha introducido delay-thread
    int seconds = 0; // Tiempo de vida del proceso
    int delay_seconds = 0; // Tiempo de espera del proceso

    int mask = 0; // Indica si se ha introducido mask
    int tam = 0; // Tamaño de los argumentos de mask
    char *mask_args[MAX_LINE/2]; // Array para almacenar los argumentos de mask

    int bgt = 0; // Indica si se ha introducido bgteam
    pthread_t tid; // Identificador del hilo
	pthread_attr_t attr; // Atributos del hilo
	pthread_attr_init(&attr); // Inicializar los atributos del hilo
    
    job *njob; /* Variable para almacenar un nuevo trabajo */

    // Inicializar la lista de trabajos
    job_list = new_list("Lista de trabajos");

    // Registrar el manejador para la señal SIGCHLD
    signal(SIGCHLD, sigchld_handler);
    // Registrar el manejador para la señal SIGHUP
    signal(SIGHUP, sighup_handler);

    // Ignorar señales en el shell principal
    ignore_terminal_signals();

    printf(PURPURA "Welcome to the shell!\n" RESET);

    while (1) {  /* Bucle principal del shell */
        printf(AZUL "COMMAND->" RESET);
        fflush(stdout); // Asegurar que el prompt se imprime inmediatamente

        // Obtener el comando del usuario
        get_command(inputBuffer, MAX_LINE, args, &background, &respawnable);

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

        // Comando interno: mostrar el trabajo actual (currjob)
        if (strcmp(args[0], "currjob") == 0) {
            if (args[1] != NULL) {
                printf(ROJO "currjob: Argumento inválido\n" RESET);
                continue; // Argumento inválido, volver al bucle principal
            }
            block_SIGCHLD(); // Bloqueamos las señales SIGCHLD para evitar condiciones de carrera
            if (empty_list(job_list)) {
                printf(ROJO "No hay trabajo actual\n" RESET); // Si la lista está vacía, no hay trabajo actual
                unblock_SIGCHLD();
                continue; // Volver al bucle principal
            }
            job *currjob = get_item_bypos(job_list, 1); // Obtenemos el primer trabajo de la lista
            if (currjob == NULL) {
                printf(ROJO "Error: No se pudo obtener el trabajo actual\n" RESET);
                unblock_SIGCHLD();
                continue; // Volver al bucle principal
            }
            printf(VERDE "Trabajo actual: PID=%d command=%s\n" RESET, currjob->pgid, currjob->command);
            unblock_SIGCHLD(); // Desbloqueamos las señales SIGCHLD
            continue; // Volver al inicio del bucle principal
        }

        // Comando interno: lanzar n veces el comando en bacground (bgteam)
        if (strcmp(args[0], "bgteam") == 0) {
            if (args[1] == NULL) {
                printf(ROJO "bgteam: Argumento inválido\n" RESET);
                continue; // Argumento inválido, volver al bucle principal
            }
            if (atoi(args[1]) <= 0) {
                printf(ROJO "bgteam: Argumento inválido\n" RESET);
                continue; // Argumento inválido, volver al bucle principal
            }
            bgt = atoi(args[1]); // Convertimos el argumento a entero
            // Reformateamos los argumentos para que el comando se ejecute correctamente
            for (int i = 2; args[i - 2]; i++) {
                args[i - 2] = args[i];
            }
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
            respawnable = 0;
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

            // Verificamos que el trabajo esté suspendido (STOPPED) o sea respawnable (RESPAWNABLE).
            if (bg_job->state != STOPPED && bg_job->state != RESPAWNABLE) {
                // Si no está suspendido, no podemos ponerlo en bg.
                printf(ROJO "bg: el trabajo seleccionado no está suspendido ni es respawnable\n" RESET);
                unblock_SIGCHLD();
                continue;
            }

            // Cambiamos el estado a BACKGROUND
            bg_job->state = BACKGROUND;
            respawnable = 0;
            // Ya no necesitamos acceso exclusivo a la lista
            unblock_SIGCHLD();

            // Enviamos SIGCONT al grupo de procesos del trabajo para reanudarlo en segundo plano.
            killpg(bg_job->pgid, SIGCONT);

            // Indicamos al usuario que el trabajo se ha reanudado en segundo plano.
            printf(VERDE "Tarea %d reanudada en segundo plano: PID: %d, Command: %s\n" RESET,
                   n, bg_job->pgid, bg_job->command);
            continue; // Volver al bucle principal
        }

        // Comando interno: limitacion de tiempo de vida
        if (strcmp(args[0], "alarm-thread") == 0) {
			if (!args[1]) { // Si no se especifica un tiempo, informamos y continuamos
				printf(ROJO "Número de segundos a esperar no especificado\n" RESET);
				continue;
			} else if (!args[2]) { // Si no se especifica un comando, informamos y continuamos
				printf(ROJO "Comando a ejecutar no especificado\n" RESET);
				continue;
			}

			if ((atoi(args[1]) > 0) || (strcmp(args[1], "0") == 0 && atoi(args[1]) == 0)) {
                seconds = atoi(args[1]); // Convertimos el argumento a entero
                thread = 1; // Indicamos que se ha creado un hilo
            } else {
                printf(ROJO "Número de segundos a esperar no válido\n" RESET);
                continue;
            }

            // Reformateamos los argumentos para que el comando se ejecute correctamente
            for (int i = 2; args[i - 2]; i++) {
                args[i - 2] = args[i];
            }
        }

        // Postergar la ejecución del comando en background
        if (strcmp(args[0], "delay-thread") == 0) {
            if (!args[1]) { // Si no se especifica un tiempo, informamos y continuamos
				printf(ROJO "Número de segundos a esperar no especificado\n" RESET);
				continue;
			} else if (!args[2]) { // Si no se especifica un comando, informamos y continuamos
				printf(ROJO "Comando a ejecutar no especificado\n" RESET);
				continue;
			}

			if ((atoi(args[1]) > 0) || (strcmp(args[1], "0") == 0 && atoi(args[1]) == 0)) {
                delay_seconds = atoi(args[1]); // Convertimos el argumento a entero
                delay = 1; // Indicamos que se ha creado un hilo
                background = 1; // Indicamos que el comando se ejecutará en segundo plano
            } else {
                printf(ROJO "Número de segundos a esperar no válido\n" RESET);
                continue;
            }

            // Reformateamos los argumentos para que el comando se ejecute correctamente
            for (int i = 2; args[i - 2]; i++) {
                args[i - 2] = args[i];
            }
        }

        // Comando interno: enmascarar señales en el hijo
        if (strcmp(args[0], "mask") == 0) {
            if (args[1] == NULL) { // No se han incluido señales a enmascarar
                printf(ROJO "No se ha incluido ninguna señal para enmascarar\n" RESET);
                continue;
            }

            // Comprobamos si no hay señales antes del -c
			if (strcmp(args[1], "-c") == 0) { //No se han incluido señales a enmascarar
				printf(ROJO "No se ha incluido ninguna señal para enmascarar\n" RESET);
				continue;
			}
			
            // Comprobamos si se ha incluido el -c
			int hay_c = 0;
			for (tam = 0; args[tam]; tam++) {
				if (strcmp(args[tam],"-c") == 0) {
					hay_c = 1;
					break;
				} 
				mask_args[tam] = args[tam]; // Array solo con los argumentos de mask
			}
			
			// No se ha incluido el -c
			if (hay_c == 0) {
				printf(ROJO "Comando no precedido con -c\n" RESET);
				continue;
			}
			
			// Comprobamos que los argumentos sean válidos
			int valido = 1;
			for (int j = 1; j < tam; j++) { // Empieza j=1 porque j=0 es "mask"
				if (atoi(mask_args[j]) <= 0) {
					valido = 0;
					break;
				}
			}
			if (!valido) {
				printf(ROJO "Argumentos no válidos para mask\n" RESET);
				continue;
			}
			
			// Si llegamos aqui es que son válidos
			for (int j = tam + 1; args[j - (tam + 1)]; j++) { // Eliminamos las posiciones de mask, +1 para quitar -c
				args[j - (tam + 1)] = args[j];
			}
            if (args[0] == NULL) { // No se ha incluido ningún comando
                printf(ROJO "No se ha incluido ningún comando\n" RESET);
                continue;
            }
			mask = 1;
		}
        
		/* =========================    COMANDOS INTERNOS    ========================= */

		/* =========================    BGTEAM    ========================= */

        for (int i = 0; i < bgt; i++) {
            bg_fork = fork();
            switch (bg_fork) {
            case -1: // Error al crear el proceso
                perror(ROJO "Error: fork() failed\n" RESET);
                exit(-1);

            case 0: // Proceso hijo
                restore_terminal_signals(); /* Restaurar señales por defecto */
                new_process_group(bg_fork); /* Crear un nuevo grupo de procesos */
                execvp(args[0], args); /* Intentar ejecutar el comando */
                exit(1); // Si falla, salimos con error

            default: /* Proceso padre */
                block_SIGCHLD();
                njob = new_job(bg_fork, args[0], BACKGROUND);
                add_job(job_list, njob); /* Bloqueamos y desbloqueamos la lista para evitar condiciones de carrera */
                printf(VERDE "Background process running -> PID: %d, Command: %s\n" RESET, bg_fork, args[0]);
                unblock_SIGCHLD();
                break;
            }
        }

        if (bgt > 0) { // Si se ha introducido bgteam, reiniciamos la variable
            bgt = 0;
            continue; // Volver al inicio del bucle principal
        }

		/* =========================    BGTEAM    ========================= */

        // Crear un nuevo proceso con fork
        pid_fork = fork();

        switch (pid_fork) {
            case -1: // Error al crear el proceso
                perror(ROJO "Error: fork() failed\n" RESET);
                exit(-1);

            case 0: /* Proceso hijo */
                restore_terminal_signals(); /* Restaurar señales por defecto */
                new_process_group(pid_fork); /* Crear un nuevo grupo de procesos */
                if (mask == 1) { // Enmascarar señales si se ha introducido mask
                    for (int j = 1; j < tam; j++) {
                        mask_signal(atoi(mask_args[j]), 0);
                    }
                }

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

                // Delay si se ha introducido delay-thread
                if (delay == 1) {
                    ThreadArgs *targs = (ThreadArgs *)malloc(sizeof(ThreadArgs)); // Creamos la estructura con los argumentos
                    targs->tiempo = delay_seconds; // Asignamos el tiempo
                    pthread_create(&tid, &attr, delay_thread, targs); // Creamos el hilo
                    pthread_join(tid, NULL); // Esperamos a que el hilo termine
                    printf(VERDE "Background process running -> PID: %d, Command: %s\n" RESET, getpid(), args[0]);
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

                if (thread == 1) { // Si se ha creado un hilo para el temporizador
                    ThreadArgs *args = (ThreadArgs *)malloc(sizeof(ThreadArgs)); // Creamos la estructura con los argumentos
                    args->tiempo = seconds; // Asignamos el tiempo
                    args->grupo = pid_fork; // Asignamos el PID del proceso
                    pthread_create(&tid, &attr, alarm_thread, args); // Creamos el hilo
                    pthread_detach(tid); // Desvinculamos el hilo
                    tid++; // Incrementamos el identificador del hilo
                    thread = 0; // Reiniciamos la variable
                }

                if (delay == 1) {
                    tid++; // Incrementamos el identificador del hilo
                    delay = 0; // Reiniciamos la variable
                }

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
                    block_SIGCHLD();
					if (respawnable == 1) {
                        njob = new_job(pid_fork, args[0], RESPAWNABLE);
                        add_resp_job(job_list, njob, args);
                        printf(VERDE "Respawnable process running -> PID: %d, Command: %s\n" RESET, pid_fork, args[0]);
                    } else {
                        njob = new_job(pid_fork, args[0], BACKGROUND);
                        add_job(job_list, njob); /* Bloqueamos y desbloqueamos la lista para evitar condiciones de carrera */
                        if (delay != 1) {
                        printf(VERDE "Background process running -> PID: %d, Command: %s\n" RESET, pid_fork, args[0]);
                        }
                    }
                    unblock_SIGCHLD();
                }
        }
    }
}