#ifndef SERVER_H
#define SERVER_H
#include <QCoreApplication>
#include <sys/socket.h>


class server
{
public:
    server(int port);
    void start ();
private:
    //QCoreApplication qApp;
    int port;
    void add_accept(struct io_uring *ring, int fd, struct sockaddr *client_addr, socklen_t *client_len);
    void add_socket_read(struct io_uring *ring, int fd, uint size);
    void add_socket_write(struct io_uring *ring, int fd, uint size);
    void add_socket_write_wait(struct io_uring *ring, int fd);
    void add_socket_write_acc(struct io_uring *ring, int fd);
    void add_socket_write_file (struct io_uring *ring, int fd,void *buf, ulong cnt);
};
/**
 * Каждое активное соединение в приложение описывается структурой conn_info.
 * fd - файловый дескриптор сокета.
 * type - описывает состояние в котором находится сокет - ждет accept, read, wait или write.
 * timer - отметка времени
 */
typedef struct conn_info {
    int fd;
    unsigned type;
    time_t timer;
} conn_info;

enum {
    ACCEPT,
    READ,
    WRITE,
    WAIT,
    WFILE
};

#endif // SERVER_H
