/**
 Класс, реализующий функционал tcp-сервера на основе библиотеки liburing
*/
#include <liburing.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include "server.h"
#include <QDebug>

#define MAX_CONNECTIONS 100
#define BACKLOG 512
#define MAX_MESSAGE_LEN 128
#define IORING_FEAT_FAST_POLL (1U << 5)
#define TIMEOUT_FOR_ACCEPTED 3000 // задержка отправки сообщения ACCEPTED (в мсек)

// Буфер для соединений.
conn_info conns[MAX_CONNECTIONS];

// Для каждого возможного соединения инициализируем буфер для чтения/записи.
char bufs[MAX_CONNECTIONS][MAX_MESSAGE_LEN];

server::server(int num_port)
{
    port = num_port;
}

void server::start()
{
    /**
     * Создаем серверный сокет и начинаем прослушивать порт.
     */

    char fileName[10];
    int fd; // file descriptor

    printf("MAX_MESSAGE_LEN=%d\n",MAX_MESSAGE_LEN);
    printf ("MAX_CONNECTIONS=%d\n",MAX_CONNECTIONS);
    printf ("TIMEOUT_FOR_ACCEPTED=%d (msek)\n",TIMEOUT_FOR_ACCEPTED);
    qDebug () <<  Q_FUNC_INFO << "MAX_MESSAGE_LEN=" << MAX_MESSAGE_LEN << "\n";
    qDebug () <<  Q_FUNC_INFO << "MAX_CONNECTIONS=" << MAX_CONNECTIONS << "\n";
    qDebug () <<  Q_FUNC_INFO << "TIMEOUT_FOR_ACCEPTED=" << TIMEOUT_FOR_ACCEPTED << "\n";

    struct sockaddr_in serv_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    int sock_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    const int val = 1;
    setsockopt(sock_listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    memset (&conns,0,sizeof(conn_info)*MAX_CONNECTIONS);
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr.s_addr = INADDR_ANY;

    assert(bind(sock_listen_fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) >= 0);
    assert(listen(sock_listen_fd, BACKLOG) >= 0);

    /**
     * Создаем инстанс io_uring, не используем никаких кастомных опций.
     * Емкость очередей SQ и CQ указываем как 4096 вхождений.
     */
    struct io_uring_params params;
    struct io_uring ring;
    memset(&params, 0, sizeof(params));

    assert(io_uring_queue_init_params(4096, &ring, &params) >= 0);

    /**
     * Проверяем наличие свойства IORING_FEAT_FAST_POLL.
     */
    if (!(params.features & IORING_FEAT_FAST_POLL)) {
        printf("IORING_FEAT_FAST_POLL not available in the kernel, quiting...\n");
        qDebug () <<  Q_FUNC_INFO << "IORING_FEAT_FAST_POLL not available in the kernel, quiting...\n";
        exit(0);
    }

    // открытие файла
    sprintf (fileName,"%d.txt",port);
    fd = open(fileName, O_RDWR | O_CREAT |O_APPEND |O_TRUNC, S_IRWXU);
    if (fd < 0) {
        printf ("ERROR open file %s:%d",fileName,errno);
        qDebug () <<  Q_FUNC_INFO << "ERROR open file " << fileName << ", errno=" << errno;
        exit(0);
    }
    printf ("file:%s, fd=%d\n",fileName,fd);

    /**
     * Добавляем в SQ первую операцию - слушаем сокет сервера для приема входящих соединений.
     */
    printf ("listen....\n");
    add_accept(&ring, sock_listen_fd, (struct sockaddr *) &client_addr, &client_len);

    /*
     * event loop
     */
    while (1) {
        struct io_uring_cqe *cqe;
        int ret;
        time_t curTime;

        /**
         * Сабмитим все SQE которые были добавлены на предыдущей итерации.
         */
        io_uring_submit(&ring);

        /**
         * Ждем когда в CQ буфере появится хотя бы одно CQE
         * или истечет таймаут TIMEOUT_FOR_ACCEPTED
         */
        ret = io_uring_wait_cqe(&ring, &cqe);
        assert (ret==0);
        curTime = time(NULL);

        /**
         * Положим все "готовые" CQE в буфер cqes.
         */
        struct io_uring_cqe *cqes[BACKLOG];
        int cqe_count = io_uring_peek_batch_cqe(&ring, cqes, sizeof(cqes) / sizeof(cqes[0]));

        for (int i = 0; i < cqe_count; ++i) {
            cqe = cqes[i];
            /**
             * В поле user_data указатель на структуру
             * в которой находится служебная информация по сокету.
             */
            struct conn_info *user_data = (struct conn_info *) io_uring_cqe_get_data(cqe);

            /**
             * Используя тип идентифицируем операцию к которой относится CQE (accept/recv/send).
             */
            unsigned type = user_data->type;
            if (type == ACCEPT) {
                int sock_conn_fd = cqe->res;

                /**
                * Если появилось новое соединение: добавляем в SQ операцию recv - читаем из клиентского сокета,
                * продолжаем слушать серверный сокет.
                */
                add_socket_read(&ring, sock_conn_fd, MAX_MESSAGE_LEN);
                add_accept(&ring, sock_listen_fd, (struct sockaddr *) &client_addr, &client_len);

                user_data->timer = 0L; // отметка времени до прихода первого сообщения

            } else if (type == READ) {
                int bytes_read = cqe->res;

                /**
                 * В случае чтения из клиентского сокета:
                 * если прочитали 0 байт - закрываем сокет
                 * если чтение успешно: добавляем в SQ операцию send -
                 */
                if (bytes_read <= 0) {
                    close(user_data->fd);
                    conns[user_data->fd].fd = 0;
                    conns[user_data->fd].type = 0;
                    conns[user_data->fd].timer = 0L;
                } else {
                    add_socket_write_wait(&ring, user_data->fd); // таймаут для ACCEPTED
                    /**
                      * Запись полученных данных в файл
                      */
                    add_socket_write_file (&ring, fd, bufs[user_data->fd], bytes_read);

                    user_data->timer = curTime;  // отметка времени поступления
                    bufs[user_data->fd][bytes_read]=(char)0;
                }
            } else if (type == WRITE) {
                /**
                * Запись в клиентский сокет окончена: добавляем в SQ операцию recv - читаем из клиентского сокета.
                */
                add_socket_read(&ring, user_data->fd, MAX_MESSAGE_LEN);
            } else if (type == WAIT) {
                /**
                * Таймаут окончен, добавляем в SQ сообщение ACCEPTED
                */
                 add_socket_write_acc(&ring, user_data->fd); // отправляем ACCEPTED
             } else if (type == WFILE) {
                // ничего не делаем
             }

            io_uring_cqe_seen(&ring, cqe);
        }
    }
    close(fd);
    io_uring_queue_exit(&ring);

}
/**
 *  Помещаем операцию accept в SQ, fd - дескриптор сокета на котором принимаем соединения.
 */
void server::add_accept(struct io_uring *ring, int fd, struct sockaddr *client_addr, socklen_t *client_len) {
    // Получаем указатель на первый доступный SQE.
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    // Хелпер io_uring_prep_accept помещает в SQE операцию ACCEPT.
    io_uring_prep_accept(sqe, fd, client_addr, client_len, 0);

    // Устанавливаем состояние серверного сокета в ACCEPT.
    conn_info *conn_i = &conns[fd];
    conn_i->fd = fd;
    conn_i->type = ACCEPT;
    conn_i->timer = 0L;

    // Устанавливаем в поле user_data указатель на socketInfo соответствующий серверному сокету.
    io_uring_sqe_set_data(sqe, conn_i);
}

/**
 *  Помещаем операцию recv в SQ.
 */
void server::add_socket_read(struct io_uring *ring, int fd, uint size) {
    // Получаем указатель на первый доступный SQE.
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    // Хелпер io_uring_prep_recv помещает в SQE операцию RECV, чтение производится в буфер соответствующий клиентскому сокету.
    io_uring_prep_recv(sqe, fd, &bufs[fd], size, 0);

    // Устанавливаем состояние клиентского сокета в READ.
    conn_info *conn_i = &conns[fd];
    conn_i->fd = fd;
    conn_i->type = READ;
    conn_i->timer = 0L; //time(NULL);

    // Устанавливаем в поле user_data указатель на socketInfo соответствующий клиентскому сокету.
    io_uring_sqe_set_data(sqe, conn_i);
}

/**
 *  Помещаем операцию send в SQ буфер. (не используется, взята из примера как основа для ...wait и ...acc)
 */
void server::add_socket_write(struct io_uring *ring, int fd, uint size) {
    // Получаем указатель на первый доступный SQE.
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    // Хелпер io_uring_prep_send помещает в SQE операцию SEND, запись производится из буфера соответствующего клиентскому сокету.
    io_uring_prep_send(sqe, fd, &bufs[fd], size, 0);

    // Устанавливаем состояние клиентского сокета в WRITE.
    conn_info *conn_i = &conns[fd];
    conn_i->fd = fd;
    conn_i->type = WRITE;

    // Устанавливаем в поле user_data указатель на socketInfo соответсвующий клиентскому сокету.
    io_uring_sqe_set_data(sqe, conn_i);

}
/**
 *  Устанавливаем задержку на отправку ACCEPTED в SQ буфер.
 */
void server::add_socket_write_wait(struct io_uring *ring, int fd) {
    struct __kernel_timespec ts; //структура для таймаута

    ts.tv_sec = TIMEOUT_FOR_ACCEPTED / 1000;
    ts.tv_nsec = (TIMEOUT_FOR_ACCEPTED % 1000) * 1000000;

    // Получаем указатель на первый доступный SQE.
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    // подготовить запрос на тайм-аут запись производится из буфера, соответствующего
    // клиентскому сокету.
    io_uring_prep_timeout(sqe,&ts,0,IORING_TIMEOUT_REALTIME);

    // Устанавливаем состояние клиентского сокета в WAIT.
    conn_info *conn_i = &conns[fd];
    conn_i->fd = fd;
    conn_i->type = WAIT;
    conn_i->timer = 0L;

    // Устанавливаем в поле user_data указатель на socketInfo соответсвующий клиентскому сокету.
    io_uring_sqe_set_data(sqe, conn_i);

}
/**
 *  Передаем ACCEPTED в SQ буфер.
 */
void server::add_socket_write_acc(struct io_uring *ring, int fd) {
    //char accept[] = "ACCEPTED\n";
    char accept[25];  // тот же ACCEPTED, только с отметкой времени для контроля на стороне клиента

    // Получаем указатель на первый доступный SQE.
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    // Хелпер io_uring_prep_send помещает в SQE операцию SEND,
    // запись производится из буфера соответствующего клиентскому сокету.
    sprintf (accept,"%lu ACCEPTED\n",time(NULL));
    io_uring_prep_send(sqe, fd, &accept, strlen(accept), 0);

    // Устанавливаем состояние клиентского сокета в WRITE.
    conn_info *conn_i = &conns[fd];
    conn_i->fd = fd;
    conn_i->type = WRITE;
    conn_i->timer = 0L;

    // Устанавливаем в поле user_data указатель на socketInfo соответсвующий клиентскому сокету.
    io_uring_sqe_set_data(sqe, conn_i);

}
/**
 * Запись полученного сообщения в файл.
 */
void server::add_socket_write_file(struct io_uring *ring, int fd, void *buf, ulong cnt) {

    // Получаем указатель на первый доступный SQE.
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    // Хелпер io_uring_prep_send помещает в SQE операцию WRITE,
    // запись производится из буфера соответствующего клиентскому сокету.
    io_uring_prep_write (sqe, fd, buf, cnt, 0);
    // Устанавливаем состояние клиентского сокета в WFILE.
    conn_info *conn_i = &conns[fd];
    conn_i->fd = fd;
    conn_i->type = WFILE;
    conn_i->timer = 0L;

    // Устанавливаем в поле user_data указатель на socketInfo соответсвующий клиентскому сокету.
    io_uring_sqe_set_data(sqe, conn_i);

}
