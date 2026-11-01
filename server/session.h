#ifndef SESSION_H
#define SESSION_H
/* Runs the request/response loop for one connected client until it quits or disconnects. */
void session_run(int fd, const char *peer);
#endif
