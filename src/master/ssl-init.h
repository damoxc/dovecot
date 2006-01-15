#ifndef __SSL_INIT_H
#define __SSL_INIT_H

#define SSL_PARAMETERS_FILENAME "ssl-parameters.dat"

void ssl_parameter_process_destroyed(pid_t pid);

void _ssl_generate_parameters(int fd, const char *fname);

void ssl_init(void);
void ssl_deinit(void);

#endif
