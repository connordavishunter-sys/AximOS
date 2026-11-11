#pragma once

#include <termios.h>

// Opens a new pseudo-terminal: master for the program that shows it, and slave as
// the terminal for the programs it runs. name, settings and size may be NULL, and
// names aren't kept, so name is always left alone.
int openpty(int *master, int *slave, char *name, const struct termios *settings, const struct winsize *size);

// Makes fd the caller's terminal and its standard input, output and error, in a
// session of its own. For a child about to run something on a slave.
int login_tty(int fd);
