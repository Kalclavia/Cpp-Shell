#include <iostream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
#include <string>
#include <sstream>
#include <cstring>
#include <termios.h>
#include <dirent.h>
#include "Tokenizer.h"

using namespace std;

// Define ANSI color codes for shell prompt styling.
#define RED "\033[1;31m"
#define GREEN "\033[1;32m"
#define YELLOW "\033[1;33m"
#define BLUE "\033[1;34m"
#define WHITE "\033[1;37m"
#define NC "\033[0m" // Reset color

// Function to get a list of files matching a given input string in the current directory
vector<string> getMatchingFiles(const string &input) {
    vector<string> matches;
    string dirPath;
    string prefix;

    size_t lastSlash = input.find_last_of('/');
    if (lastSlash == string::npos) {
        dirPath = ".";
        prefix = input;
    } else {
        dirPath = input.substr(0, lastSlash);
        prefix = input.substr(lastSlash + 1);
    }

    DIR *dir = opendir(dirPath.c_str());
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != nullptr) {
            string fname = entry->d_name;
            if (fname.find(prefix) == 0) {
                matches.push_back(fname);
            }
        }
        closedir(dir);
    }
    return matches;
}

// Variables for command history and index
vector<string> commandHistory;
int currentHistoryIndex = -1;

// Function to get a single character from the input (used for handling special keys)
int getkey() {
    int character;
    struct termios orig_term_attr;
    struct termios new_term_attr;

    tcgetattr(fileno(stdin), &orig_term_attr);
    memcpy(&new_term_attr, &orig_term_attr, sizeof(struct termios));
    new_term_attr.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(fileno(stdin), TCSANOW, &new_term_attr);
    character = fgetc(stdin);
    tcsetattr(fileno(stdin), TCSANOW, &orig_term_attr);

    return character;
}

// Function to execute a single command
void executeCommand(const Command &cmd) {
    // Fork to create a child process to execute the command.
    pid_t pid = fork();

    if (pid < 0) { // error checking 
        perror("fork");
        exit(2);
    }

    if (pid == 0) {
        // Child process: redirect input and output, and execute the command.
        if (!cmd.in_file.empty()) {
            int fd_in = open(cmd.in_file.c_str(), O_RDONLY);
            if (fd_in < 0) {
                perror("open");
                exit(2);
            }
            dup2(fd_in, STDIN_FILENO);
            close(fd_in);
        }

        if (!cmd.out_file.empty()) {
            int fd_out = open(cmd.out_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd_out < 0) {
                perror("open");
                exit(2);
            }
            dup2(fd_out, STDOUT_FILENO);
            close(fd_out);
        }

        // Convert arguments to a format suitable for execvp.
        vector<char *> args;
        for (const string &arg : cmd.args) {
            args.push_back(const_cast<char *>(arg.c_str()));
        }
        args.push_back(nullptr);

        // Execute the command.
        execvp(args[0], &args[0]);
        perror("execvp");
        exit(2);
    } else {
        // Parent process: wait for the child to finish.
        int status = 0;
        waitpid(pid, &status, 0);
        if (status > 1) {
            exit(status);
        }
    }
}

int main() {
    // Store the previous directory for the 'cd -' command.
    string previousDir = "";

    // Vector object to store background PIDs
    vector<pid_t> backgroundProcessStore;

    for (;;) {
        // Initialize string to store command
        string thisCommand = "";

        // Get path to current working directory (CWD) and username
        char buffer[256];
        getcwd(buffer, sizeof(buffer));

        // Get the current date/time, username
        char *username = getlogin();
        char timeBuf[80];
        time_t currentTime = time(0);
        struct tm tstruct = *localtime(&currentTime);
        strftime(timeBuf, sizeof(timeBuf), "%b %d %H:%M:%S", &tstruct);

        // Display the shell prompt with the current date/time, username, and current directory.
        cout << GREEN << timeBuf << " " << username << ":" << BLUE << buffer << YELLOW << "$ " << NC;

        // Get user input.
        string input;
        getline(cin, input);

        // Handling background processes (&)
        for(size_t i = 0; i < backgroundProcessStore.size();) {
            int status;
            if (waitpid(backgroundProcessStore[i], &status, WNOHANG) == backgroundProcessStore[i]) {
                backgroundProcessStore.erase(backgroundProcessStore.begin() + i);
            } else {
                i++;
            }
        }

        istringstream commandStream(input);

        // Process commands separated by semicolons.
        while (getline(commandStream, thisCommand, ';')) {
            // Trim leading and trailing whitespace from the command.
            thisCommand = thisCommand.substr(thisCommand.find_first_not_of(" "), thisCommand.find_last_not_of(" ") - thisCommand.find_first_not_of(" ") + 1);

            if (thisCommand == "exit") {
                // Exit the shell.
                cout << RED << "Exiting shell..." << NC << endl;
                return 0;
            }

            if (thisCommand.substr(0, 3) == "cd ") {
                // Change directory using the 'cd' command.
                string targetDir = thisCommand.substr(3);

                if (targetDir == "-") {
                    if (previousDir == "") {
                        cerr << "No previous directory to change to." << endl;
                        continue;
                    }
                    targetDir = previousDir;
                }

                char currentDir[1024];
                getcwd(currentDir, sizeof(currentDir));
                previousDir = currentDir;

                if (chdir(targetDir.c_str()) != 0) {
                    perror("cd");
                }

                continue;
            }

            // Tokenize the command.
            Tokenizer tknr(thisCommand);

            if (tknr.hasError()) {
                // If there was an error in tokenization, continue to the next prompt.
                continue;
            }

            // Execute the command(s).
            if (tknr.commands.size() == 1) {
                // Single command without piping
                if (tknr.commands[0]->isBackground()) {
                    // Background process
                    pid_t pid = fork();
                    if (pid < 0) {
                        perror("fork");
                        exit(2);
                    }
                    if (pid == 0) {
                        // Child process
                        executeCommand(*tknr.commands[0]);
                        exit(0);
                    } else {
                        // Parent process: add PID to backgroundProcessStore
                        backgroundProcessStore.push_back(pid);
                    }
                } else {
                    // Foreground process
                    executeCommand(*tknr.commands[0]);
                }
            } else {
                // Multiple commands with piping
                int prev_pipe_fd[2] = {-1, -1};
                for (size_t i = 0; i < tknr.commands.size(); i++) {
                    int pipe_fd[2];
                    int in_fd = -1, out_fd = -1;

                    if (i != tknr.commands.size() - 1) {
                        if (pipe(pipe_fd) < 0) {
                            perror("pipe");
                            exit(2);
                        }
                    }

                    if (!tknr.commands[i]->in_file.empty()) {
                        in_fd = open(tknr.commands[i]->in_file.c_str(), O_RDONLY);
                        if (in_fd < 0) {
                            perror("open");
                            continue;
                        }
                    }

                    if (!tknr.commands[i]->out_file.empty()) {
                        out_fd = open(tknr.commands[i]->out_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                        if (out_fd < 0) {
                            perror("open");
                            continue;
                        }
                    }

                    pid_t pid = fork();

                    if (pid < 0) { // error checking 
                        perror("fork");
                        exit(2);
                    }

                    if (pid == 0) {
                        // Child process: redirect input and output, and execute the command.
                        if (in_fd != -1) {
                            dup2(in_fd, STDIN_FILENO);
                            close(in_fd);
                        } else if (prev_pipe_fd[0] != -1) {
                            dup2(prev_pipe_fd[0], STDIN_FILENO);
                        }

                        if (out_fd != -1) {
                            dup2(out_fd, STDOUT_FILENO);
                            close(out_fd);
                        } else if (i != tknr.commands.size() - 1) {
                            dup2(pipe_fd[1], STDOUT_FILENO);
                        }

                        if (prev_pipe_fd[0] != -1) {
                            close(prev_pipe_fd[0]);
                        }
                        if (prev_pipe_fd[1] != -1) {
                            close(prev_pipe_fd[1]);
                        }
                        if (pipe_fd[0] != -1) {
                            close(pipe_fd[0]);
                        }
                        if (pipe_fd[1] != -1) {
                            close(pipe_fd[1]);
                        }

                        // Convert arguments to a format suitable for execvp.
                        vector<char *> args;
                        for (const string &arg : tknr.commands[i]->args) {
                            args.push_back(const_cast<char *>(arg.c_str()));
                        }
                        args.push_back(nullptr);

                        // Execute the command.
                        execvp(args[0], &args[0]);
                        perror("execvp");
                        exit(2);
                    } else {
                        // Parent process: wait for the child to finish and manage pipes.
                        if (in_fd != -1) {
                            close(in_fd);
                        }
                        if (out_fd != -1) {
                            close(out_fd);
                        }
                        if (prev_pipe_fd[0] != -1) {
                            close(prev_pipe_fd[0]);
                        }
                        if (prev_pipe_fd[1] != -1) {
                            close(prev_pipe_fd[1]);
                        }

                        int status = 0;
                        waitpid(pid, &status, 0);
                        if (status > 1) {
                            exit(status);
                        }

                        prev_pipe_fd[0] = pipe_fd[0];
                        prev_pipe_fd[1] = pipe_fd[1];
                    }
                }
            }
        }
    }

    return 0;
}