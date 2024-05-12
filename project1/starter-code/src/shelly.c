#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h> //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>

#include <sys/types.h>
#include <dirent.h>
#include <curl/curl.h>

const char *sysname = "furshell";

enum return_codes {
	SUCCESS = 0,
	EXIT = 1,
	UNKNOWN = 2,
};

struct command_t {
	char *name;
	bool background;
	bool auto_complete;
	int arg_count;
	char **args;
	char *redirects[3]; // in/out redirection
	struct command_t *next; // for piping
};

/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t *command) {
	int i = 0;
	printf("Command: <%s>\n", command->name);
	printf("\tIs Background: %s\n", command->background ? "yes" : "no");
	printf("\tNeeds Auto-complete: %s\n",
		   command->auto_complete ? "yes" : "no");
	printf("\tRedirects:\n");

	for (i = 0; i < 3; i++) {
		printf("\t\t%d: %s\n", i,
			   command->redirects[i] ? command->redirects[i] : "N/A");
	}

	printf("\tArguments (%d):\n", command->arg_count);

	for (i = 0; i < command->arg_count; ++i) {
		printf("\t\tArg %d: %s\n", i, command->args[i]);
	}

	if (command->next) {
		printf("\tPiped to:\n");
		print_command(command->next);
	}
}

/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command) {
	if (command->arg_count) {
		for (int i = 0; i < command->arg_count; ++i)
			free(command->args[i]);
		free(command->args);
	}

	for (int i = 0; i < 3; ++i) {
		if (command->redirects[i])
			free(command->redirects[i]);
	}

	if (command->next) {
		free_command(command->next);
		command->next = NULL;
	}

	free(command->name);
	free(command);
	return 0;
}

/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt() {
	char cwd[1024], hostname[1024];
	gethostname(hostname, sizeof(hostname));
	getcwd(cwd, sizeof(cwd));
	printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
	return 0;
}

/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command) {
	const char *splitters = " \t"; // split at whitespace
	int index, len;
	len = strlen(buf);

	// trim left whitespace
	while (len > 0 && strchr(splitters, buf[0]) != NULL) {
		buf++;
		len--;
	}

	while (len > 0 && strchr(splitters, buf[len - 1]) != NULL) {
		// trim right whitespace
		buf[--len] = 0;
	}

	// auto-complete
	if (len > 0 && buf[len - 1] == '?') {
		command->auto_complete = true;
	}

	// background
	if (len > 0 && buf[len - 1] == '&') {
		command->background = true;
	}

	char *pch = strtok(buf, splitters);


	if (pch == NULL) {
		command->name = (char *)malloc(1);
		command->name[0] = 0;
	} else {
		command->name = (char *)malloc(strlen(pch) + 1);
		strcpy(command->name, pch);
	}

	command->args = (char **)malloc(sizeof(char *));

	int redirect_index;
	int arg_index = 0;
	char temp_buf[1024], *arg;

	while (1) {
		// tokenize input on splitters
		pch = strtok(NULL, splitters);
		if (!pch)
			break;
		arg = temp_buf;
		strcpy(arg, pch);
		len = strlen(arg);

		// empty arg, go for next
		if (len == 0) {
			continue;
		}

		// trim left whitespace
		while (len > 0 && strchr(splitters, arg[0]) != NULL) {
			arg++;
			len--;
		}

		// trim right whitespace
		while (len > 0 && strchr(splitters, arg[len - 1]) != NULL) {
			arg[--len] = 0;
		}

		// empty arg, go for next
		if (len == 0) {
			continue;
		}

		// piping to another command
		if (strcmp(arg, "|") == 0) {
			struct command_t *c = malloc(sizeof(struct command_t));
			int l = strlen(pch);
			pch[l] = splitters[0]; // restore strtok termination
			index = 1;
			while (pch[index] == ' ' || pch[index] == '\t')
				index++; // skip whitespaces

			parse_command(pch + index, c);
			pch[l] = 0; // put back strtok termination
			command->next = c;
			continue;
		}

		// background process
		if (strcmp(arg, "&") == 0) {
			// handled before
			continue;
		}

		// handle input redirection
		redirect_index = -1;
		if (arg[0] == '<') {
			redirect_index = 0;
		}

		if (arg[0] == '>') {
			if (len > 1 && arg[1] == '>') {
				redirect_index = 2;
				arg++;
				len--;
			} else {
				redirect_index = 1;
			}
		}

		if (redirect_index != -1) {
			command->redirects[redirect_index] = malloc(len);
			strcpy(command->redirects[redirect_index], arg + 1);
			continue;
		}

		// normal arguments
		if (len > 2 &&
			((arg[0] == '"' && arg[len - 1] == '"') ||
			 (arg[0] == '\'' && arg[len - 1] == '\''))) // quote wrapped arg
		{
			arg[--len] = 0;
			arg++;
		}

		command->args =
			(char **)realloc(command->args, sizeof(char *) * (arg_index + 1));

		command->args[arg_index] = (char *)malloc(len + 1);
		strcpy(command->args[arg_index++], arg);
	}
	command->arg_count = arg_index;

	// increase args size by 2												
	command->args = (char **)realloc(
		command->args, sizeof(char *) * (command->arg_count += 2));			

	// shift everything forward by 1										
	for (int i = command->arg_count - 2; i > 0; --i) {						
		command->args[i] = command->args[i - 1];							
	}

	// set args[0] as a copy of name										
	command->args[0] = strdup(command->name);								

	// set args[arg_count-1] (last) to NULL									
	command->args[command->arg_count - 1] = NULL;							

	return 0;
}

void prompt_backspace() {
	putchar(8); // go back 1
	putchar(' '); // write empty over
	putchar(8); // go back 1 again
}

/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command) {
	size_t index = 0;
	char c;
	char buf[4096];
	static char oldbuf[4096];

	// tcgetattr gets the parameters of the current terminal
	// STDIN_FILENO will tell tcgetattr that it should write the settings
	// of stdin to oldt
	static struct termios backup_termios, new_termios;
	tcgetattr(STDIN_FILENO, &backup_termios);
	new_termios = backup_termios;
	// ICANON normally takes care that one line at a time will be processed
	// that means it will return if it sees a "\n" or an EOF or an EOL
	new_termios.c_lflag &=
		~(ICANON |
		  ECHO); // Also disable automatic echo. We manually echo each char.
	// Those new settings will be set to STDIN
	// TCSANOW tells tcsetattr to change attributes immediately.
	tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);

	show_prompt();


	buf[0] = 0;

	while (1) {
		c = getchar();
		// printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

		// handle tab
		if (c == 9) {
			buf[index++] = '?'; // autocomplete
			break;
		}

		// handle backspace
		if (c == 127) {
			if (index > 0) {
				prompt_backspace();
				index--;
			}
			continue;
		}

		if (c == 27 || c == 91 || c == 66 || c == 67 || c == 68) {
			continue;
		}

		// up arrow
		if (c == 65) {
			while (index > 0) {
				prompt_backspace();
				index--;
			}

			char tmpbuf[4096];
			printf("%s", oldbuf);
			strcpy(tmpbuf, buf);
			strcpy(buf, oldbuf);
			strcpy(oldbuf, tmpbuf);
			index += strlen(buf);
			continue;
		}

		putchar(c); // echo the character
		buf[index++] = c;
		if (index >= sizeof(buf) - 1)
			break;
		if (c == '\n') // enter key
			break;
		if (c == 4) // Ctrl+D
			return EXIT;
	}

	// trim newline from the end
	if (index > 0 && buf[index - 1] == '\n') {
		index--;
	}

	// null terminate string
	buf[index++] = '\0';

	strcpy(oldbuf, buf);

	parse_command(buf, command);

	// print_command(command); // DEBUG: uncomment for debugging

	// restore the old settings
	tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
	return SUCCESS;
}

int process_command(struct command_t *command);

int main() {
	while (1) {
		struct command_t *command = malloc(sizeof(struct command_t));

		// set all bytes to 0
		memset(command, 0, sizeof(struct command_t));

		int code;
		code = prompt(command);
		if (code == EXIT) {
			break;
		}

		code = process_command(command);
		if (code == EXIT) {
			break;
		}

		free_command(command);
	}

	printf("\n");
	return 0;
}

int process_command(struct command_t *command) {
	int r;

	if (strcmp(command->name, "") == 0) {
		return SUCCESS;
	}

	if (strcmp(command->name, "exit") == 0) {
		return EXIT;
	}

	if (strcmp(command->name, "cd") == 0) {
		if (command->arg_count > 0) {
			r = chdir(command->args[0]);
			if (r == -1) {
				printf("-%s: %s: %s\n", sysname, command->name,
					   strerror(errno));
			}

			return SUCCESS;
		}
	}

	pid_t pid = fork();
    if (pid == 0) { // Child process
        // Handle I/O redirection
        if (command->redirects[0] != NULL) {  // Input redirection
            int fd0 = open(command->redirects[0], O_RDONLY);
            if (fd0 == -1) perror("open");
            dup2(fd0, STDIN_FILENO);
            close(fd0);
        }
        if (command->redirects[1] != NULL) {  // Output redirection (truncate)
            int fd1 = open(command->redirects[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd1 == -1) perror("open");
            dup2(fd1, STDOUT_FILENO);
            close(fd1);
        }
        if (command->redirects[2] != NULL) {  // Output redirection (append)
            int fd2 = open(command->redirects[2], O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd2 == -1) perror("open");
            dup2(fd2, STDOUT_FILENO);
            close(fd2);
        }

        // Execute the command using execvp to handle PATH resolution
        execvp(command->name, command->args);
        perror("execvp"); // Exec only returns on error
        exit(EXIT_FAILURE);
    } else if (pid > 0) { // Parent process
        if (!command->background) {
            waitpid(pid, NULL, 0); // Wait for the child process to finish if not a background process
        }
    } else {
        perror("fork"); // Fork failed
        return UNKNOWN;
    }
    

	// TODO: your implementation here
	if (strcmp(command->name, "uniq") == 0) {
		return process_uniq_command(command);
	}

	if (strcmp(command->name, "interrect") == 0) {
        return handle_interrect_command(command);
    }

	if (strcmp(command->name, "psvis") == 0) {
        return handle_psvis_command(command);
    }

	if (strcmp(command->name, "hdiff") == 0) {
    	return process_hdiff_command(command);
	}

	if (strcmp(command->name, "psvis") == 0) {
		return process_psvis_command(command);
	}

	if (strcmp(command->name, "mtv") == 0) {
        return process_mtv_command(command);
    }

	printf("-%s: %s: command not found\n", sysname, command->name);
	return UNKNOWN;
}

int process_uniq_command(struct command_t *command) {
    if (command->arg_count < 2) {
        printf("Error: No file provided.\n");
        return UNKNOWN;
    }
    FILE *f = fopen(command->args[command->arg_count-1], "r");
    if (!f) {
        perror("Error opening file");
        return UNKNOWN;
    }

    char line[512];
    char *uniq_lines[1000]; // More room for unique lines
    int count = 0;
    bool count_occurrences = false;

    if (command->arg_count == 3 && (strcmp(command->args[1], "-c") == 0 || strcmp(command->args[1], "--count") == 0)) {
        count_occurrences = true;
    }

    while (fgets(line, sizeof(line), f)) {
        if (line[strlen(line) - 1] == '\n') {
            line[strlen(line) - 1] = '\0'; // Strip newline
        }

        bool found = false;
        for (int i = 0; i < count; i++) {
            if (strcmp(uniq_lines[i], line) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            uniq_lines[count++] = strdup(line); // Store unique line
        }
    }

    for (int i = 0; i < count; i++) {
        if (count_occurrences) {
            int occurrences = 0;
            rewind(f); // Go back to the beginning of the file
            char temp[512];
            while (fgets(temp, sizeof(temp), f)) {
                if (temp[strlen(temp) - 1] == '\n') {
                    temp[strlen(temp) - 1] = '\0';
                }
                if (strcmp(uniq_lines[i], temp) == 0) {
                    occurrences++;
                }
            }
            printf("%d %s\n", occurrences, uniq_lines[i]);
        } else {
            printf("%s\n", uniq_lines[i]);
        }
    }

    for (int i = 0; i < count; i++) {
        free(uniq_lines[i]); // Free allocated strings
    }
    fclose(f);
    return SUCCESS;
}

int handle_interrect_command(struct command_t *command) {
    if (command->arg_count < 1) {
        printf("Usage: %s <minutes>\n", command->name);
        return UNKNOWN;
    }

    int min = atoi(command->args[0]);
    if (min <= 0) {
        printf("Invalid interval: %s. Please provide a positive integer.\n", command->args[0]);
        return UNKNOWN;
    }

    FILE *file_ptr = fopen("temp.txt", "w");
    if (!file_ptr) {
        perror("Failed to open file");
        return UNKNOWN;
    }

    fprintf(file_ptr, "*/%d * * * * /usr/games/fortune | /usr/bin/espeak\n", min);
    fclose(file_ptr);

    pid_t child = fork();
    if (child == 0) {
        char *cronArgs[] = {"/usr/bin/crontab", "temp.txt", NULL};
        execv("/usr/bin/crontab", cronArgs);
        perror("execv");
        exit(EXIT_FAILURE);
    } else if (child > 0) {
        int childStatus;
        waitpid(child, &childStatus, 0);
        if (WIFEXITED(childStatus) && WEXITSTATUS(childStatus) != 0) {
            printf("Failed to set cron job.\n");
        }
    } else {
        perror("fork");
        remove("temp.txt");
        return UNKNOWN;
    }

    remove("temp.txt");
    return SUCCESS;
}

int handle_psvis_command(struct command_t *command) {
    if (command->arg_count != 2) {
        printf("Psvis requires two arguments.\n");
        return UNKNOWN; // Changed from EXIT to UNKNOWN for consistency with other command failures
    }

    char insmodCmd[256];
    snprintf(insmodCmd, sizeof(insmodCmd), "sudo insmod mymodule.ko %s", command->args[0]);
    system(insmodCmd); // Load the module with the first argument

    char rmmodCmd[] = "sudo rmmod mymodule";
    system(rmmodCmd); // Unload the module

    char dmesgCmd[256];
    snprintf(dmesgCmd, sizeof(dmesgCmd), "sudo dmesg -c > %s", command->args[1]);
    system(dmesgCmd); // Capture dmesg output into the second argument

    return SUCCESS;
}

// // Function to list all files in the current directory
// void list_directory_contents() {
//     DIR *d;
//     struct dirent *dir;
//     d = opendir(".");
//     if (d) {
//         while ((dir = readdir(d)) != NULL) {
//             printf("%s\n", dir->d_name);
//         }
//         closedir(d);
//     }
// }

// // Function to find command completions
// void auto_complete(char *input) {
//     const char *commands[] = {"cd", "exit", "uniq", "interrect", "psvis", "hdiff", NULL};  // Add all your commands here
//     int partial = 0;
//     printf("\nPossible completions:\n");
//     for (int i = 0; commands[i] != NULL; i++) {
//         if (strncmp(commands[i], input, strlen(input)) == 0) {
//             printf("%s\n", commands[i]);
//             partial++;
//         }
//     }
//     if (partial == 1) {
//         strcpy(input, commands[partial - 1]); // auto-complete for single match
//     }
// }

// Function to compare two files line by line
int compare_text_files(FILE *file1, FILE *file2) {
    char line1[1024], line2[1024];
    int diff_count = 0, line_number = 1;
    
    while (fgets(line1, sizeof(line1), file1) && fgets(line2, sizeof(line2), file2)) {
        if (strcmp(line1, line2) != 0) {
            printf("file1.txt:Line %d: %s", line_number, line1);
            printf("file2.txt:Line %d: %s", line_number, line2);
            diff_count++;
        }
        line_number++;
    }

    if (!feof(file1) || !feof(file2)) {
        printf("Files differ in length.\n");
        return 1;
    }

    if (diff_count == 0)
        printf("The two text files are identical\n");
    else
        printf("%d different lines found\n", diff_count);

    return diff_count;
}

// Function to compare two files byte by byte
int compare_binary_files(FILE *file1, FILE *file2) {
    char byte1, byte2;
    int diff_bytes = 0;

    while (fread(&byte1, sizeof(char), 1, file1) && fread(&byte2, sizeof(char), 1, file2)) {
        if (byte1 != byte2)
            diff_bytes++;
    }

    if (!feof(file1) || !feof(file2)) {
        printf("Files differ in length.\n");
        return 1;
    }

    if (diff_bytes == 0)
        printf("The two files are identical\n");
    else
        printf("The two files are different in %d bytes\n", diff_bytes);

    return diff_bytes;
}

// hdiff command function
int process_hdiff_command(struct command_t *command) {
    if (command->arg_count != 4) {
        printf("Usage: hdiff [-a | -b] <file1> <file2>\n");
        return UNKNOWN;
    }

    FILE *file1 = fopen(command->args[2], "r");
    FILE *file2 = fopen(command->args[3], "r");

    if (!file1 || !file2) {
        perror("Error opening files");
        if (file1) fclose(file1);
        if (file2) fclose(file2);
        return UNKNOWN;
    }

    int result;
    if (strcmp(command->args[1], "-a") == 0)
        result = compare_text_files(file1, file2);
    else if (strcmp(command->args[1], "-b") == 0)
        result = compare_binary_files(file1, file2);
    else {
        printf("Invalid option. Use -a for text comparison or -b for binary comparison.\n");
        result = UNKNOWN;
    }

    fclose(file1);
    fclose(file2);
    return result;
}


// Mock function to simulate reading process data
void simulate_process_data(const char* filepath) {
    // Normally, this function would collect data from the kernel module
    FILE* fp = fopen(filepath, "w");
    if(fp) {
        fprintf(fp, "1000 0\n1001 1000\n1002 1000\n1003 1001\n1004 1002\n");
        fclose(fp);
    }
}

// Function to visualize the process tree in ASCII
void visualize_process_tree(const char* filepath) {
    FILE* fp = fopen(filepath, "r");
    if (!fp) {
        perror("Failed to open file");
        return;
    }

    int pid, parent_pid;
    printf("Process Tree:\n");
    while (fscanf(fp, "%d %d", &pid, &parent_pid) == 2) {
        printf("PID: %d, Parent PID: %d\n", pid, parent_pid);
        // Further processing to visualize in a structured format could be added here
    }

    fclose(fp);
}

// Integrate visualization into the psvis command function
int process_psvis_command(struct command_t *command) {
    if (command->arg_count != 3) {
        printf("Usage: psvis <PID> <output file>\n");
        return UNKNOWN;
    }

    // Call to kernel module to fetch process data, simulated here
    simulate_process_data(command->args[2]);

    // Now visualize the process tree from the data file
    visualize_process_tree(command->args[2]);

    return SUCCESS;
}

// Struct to hold the tax rates for different years
typedef struct {
    int start_year;
    int end_year;
    int tax_rate;
} YearTax;

// Struct to hold the engine volumes and their corresponding tax rates
typedef struct {
    int min_volume;
    int max_volume;
    YearTax year_taxes[5];  // Assuming there are 5 different year ranges for each volume bracket
} VolumeTax;

// Initialize the tax brackets
VolumeTax tax_brackets[] = {
    {0, 1300, {{2021, 2024, 3359}, {2018, 2020, 2343}, {2013, 2017, 1308}, {2010, 2012, 987}, {0, 2009, 347}}},
    {1301, 1600, {{2021, 2024, 5851}, {2018, 2020, 4387}, {2013, 2017, 2544}, {2010, 2012, 1798}, {0, 2009, 690}}},
    {1601, 1800, {{2021, 2024, 11374}, {2018, 2020, 8894}, {2013, 2017, 5227}, {2010, 2012, 3189}, {0, 2009, 1235}}},
    {1801, 2000, {{2021, 2024, 17920}, {2018, 2020, 13800}, {2013, 2017, 8111}, {2010, 2012, 4828}, {0, 2009, 1898}}},
    {2001, 2500, {{2021, 2024, 26885}, {2018, 2020, 19517}, {2013, 2017, 12193}, {2010, 2012, 7282}, {0, 2009, 2880}}},
    // Add other brackets following the same pattern
    {4001, INT_MAX, {{2021, 2024, 146932}, {2018, 2020, 110177}, {2013, 2017, 65252}, {2010, 2012, 29326}, {0, 2009, 11374}}}
};

// Function to calculate the MTV based on engine volume and year
int calculate_mtv(int volume, int year) {
    for (int i = 0; i < sizeof(tax_brackets) / sizeof(tax_brackets[0]); i++) {
        if (volume >= tax_brackets[i].min_volume && volume <= tax_brackets[i].max_volume) {
            for (int j = 0; j < 5; j++) {  // Assuming each volume bracket has 5 year ranges
                if (year >= tax_brackets[i].year_taxes[j].start_year && year <= tax_brackets[i].year_taxes[j].end_year) {
                    return tax_brackets[i].year_taxes[j].tax_rate;
                }
            }
        }
    }
    return -1;  // Return -1 if no tax bracket is found (should not happen)
}

// MTV command function
int process_mtv_command(struct command_t *command) {
    if (command->arg_count != 3) {
        printf("Usage: mtv <engine volume> <year>\n");
        return UNKNOWN;
    }

    int volume = atoi(command->args[1]);
    int year = atoi(command->args[2]);
    int tax = calculate_mtv(volume, year);

    if (tax != -1) {
        printf("The MTV for a %d cm^3 engine from year %d is %d TL.\n", volume, year, tax);
    } else {
        printf("No tax information available for the specified volume and year.\n");
    }

    return SUCCESS;
}

/**
 * Displays detailed information about a command including its arguments,
 * redirections, and linked commands (for pipelining).
 * 
 * @param command The command structure to be displayed.
 */
void display_command_details(const struct command_t *command) {
    printf("Command: <%s>\n", command->name);
    printf("\tIs Background: %s\n", command->background ? "yes" : "no");
    printf("\tNeeds Auto-complete: %s\n", command->auto_complete ? "yes" : "no");
    printf("\tRedirects:\n");

    for (int i = 0; i < 3; i++) {
        printf("\t\t%d: %s\n", i, command->redirects[i] ? command->redirects[i] : "N/A");
    }

    printf("\tArguments (%d):\n", command->arg_count);
    for (int i = 0; i < command->arg_count; i++) {
        printf("\t\tArg %d: %s\n", i, command->args[i]);
    }

    if (command->next) {
        printf("\tPiped to:\n");
        display_command_details(command->next);
    }
}
