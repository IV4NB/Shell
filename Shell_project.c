/**
UNIX Shell Project

Sistemas Operativos
Grados I. Informatica, Computadores & Software
Dept. Arquitectura de Computadores - UMA

**/

#include <string.h>
#include <errno.h>
#include "job_control.h" 
#define ROJO "\x1b[31;1;1m"			
#define NEGRO "\x1b[0m"
#define VERDE "\x1b[32;1;1m"
#define AZUL "\x1b[34;1;1m"
#define CIAN "\x1b[36;1;1m"
#define MARRON "\x1b[33;1;1m"
#define PURPURA "\x1b[35;1;1m"
#define RESET "\033[0m"

#define MAX_LINE 256 /* 256 chars per line, per command, should be enough. */

// -----------------------------------------------------------------------
//                            MAIN          
// -----------------------------------------------------------------------

int main(void)
{
	char inputBuffer[MAX_LINE]; /* buffer to hold the command entered */
	int background;             /* equals 1 if a command is followed by '&' */
	char *args[MAX_LINE/2];     /* command line (of 256) has max of 128 arguments */
	// probably useful variables:
	int pid_fork, pid_wait; /* pid for created and waited process */
	int status;             /* status returned by wait */
	enum status status_res; /* status processed by analyze_status() */
	int info;				/* info processed by analyze_status() */
	int pid_shell = getpid();

	ignore_terminal_signals();  /* shell ignores signals */
	printf(PURPURA "Welcome to the shell!\n" RESET);

	while (1)   /* Program terminates normally inside get_command() after ^D is typed*/
	{   		
		printf("COMMAND->");
		fflush(stdout);
		
		get_command(inputBuffer, MAX_LINE, args, &background);  /* get next command */
		
		if(args[0]==NULL) continue;   /* ignore empty command */

		pid_fork = fork();

		switch (pid_fork)
		{
			case -1:
				perror(ROJO"Error: fork() failed\n"RESET);
				exit(-1);
				break;
			case 0: /* Proceso hijo */
				restore_terminal_signals(); /* Restaurar señales por defecto */
				execvp(args[0], args); /* Intentar ejecutar el comando */
				// Si execvp falla, manejar distintos errores
				switch (errno) {
					case ENOENT: /* Archivo no encontrado */
						fprintf(stderr, ROJO "error: command not found: %s\n" RESET, args[0]);
						exit(127); /* Código estándar para comando no encontrado */
						break;
					case EACCES: /* Permisos insuficientes */
						fprintf(stderr, ROJO "error: permission denied: %s\n" RESET, args[0]);
						exit(126); /* Código estándar para error de permisos */
						break;
					case ENOEXEC: /* No es un ejecutable válido */
						fprintf(stderr, ROJO "error: not an executable: %s\n" RESET, args[0]);
						exit(126); /* Código estándar para error de permisos */
						break;
					default: /* Otros errores del sistema */
						fprintf(stderr, ROJO "error: execvp failed (%s): %s\n" RESET, strerror(errno), args[0]);
						exit(EXIT_FAILURE); /* Código genérico de error */
						break;
    }

			default: /* Parent process */
				if (background == 0) { /* Si es foreground */
					pid_wait = waitpid(pid_fork, &status, WUNTRACED);
					status_res = analyze_status(status, &info); /* Analizar el estado del proceso hijo */
					printf(VERDE "Foreground pid: %d, Command: %s, Status: %s, Info: %d\n" RESET, 
						pid_wait, args[0], status_strings[status_res], WEXITSTATUS(status));
				} 
				else {
					/* Si es background, simplemente informar */
					printf(VERDE "Background process running -> PID: %d, Command: %s\n" RESET, pid_fork, args[0]);
				}
				break;
		}

		/* the steps are:
			 (1) fork a child process using fork()
			 (2) the child process will invoke execvp()
			 (3) if background == 0, the parent will wait, otherwise continue 
			 (4) Shell shows a status message for processed command
			 (5) loop returns to get_commnad() function
		*/

	} // end while
}
