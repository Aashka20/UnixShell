# UnixShell
tsh.c is a small Unix shell program that provides a command-line interface for users to interact with their operating system, run programs, and manage job states. Also support features such as input/output redirection, signals and pipes. 

# Features
1. Command Execution: Executes commands entered by the user, either built-in commands or external executable files.
2. Input/Output Direction: Supports input redirection with "<" and output redirection with ">".
3. Background and Foreground Execution: Runs jobs either in the background or foreground based on the presence of an ampersand ('&') at the end of the command line.
4. Job Control: Provides built-in commands ('quit', 'bg', 'fg', 'jobs') for managing background and foreground jobs.
5. Signal Handling: Catching signals such as SIGINT (ctrl-C) and SIGSTP (ctrl-Z) to control the execution of jobs.
6. Job Listing: List all background jobs
7. Zombie process clean-up: Cleans up the zombie process after job termination.

# Usage
1. Compilation: compile the tsh program by running 'make'.
2. Execution: Run the shell program by typing './tsh' in the terminal.
3. Command Entry: Enter commands at the shell prompt, following standard Unix shell conventions
4. Built-in Commands:
   - 'quit' - terminate the shell.
   - 'bg <job> - Move a job to the foreground (job can be a process ID or a job ID).
   - 'fg <job> - Move a job to the background (job can be a pocess ID or a job ID).
   - 'jobs' - List all background jobs.
5. Input/Output Redirection: Use '<' and '>' symbols for input and output redirection, respectively.
6. Signal Handling: Typing ctrl-c sends a SIGINT signal, and ctrl-z sends a SIGTSTP signal to the current foreground job.

# Testing
Trace files are provided and shell driver 'sdriver.pl' for testing correction of shell execution. The trace files are run to compare the output of the implemented shell with the reference trace files provided. The reference trace files contain the correct output of shell upon running a sequence of commands. 
