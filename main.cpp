/**
  * Программа TCP-сервер
  * принимает сообщения с использованием библиотеки liburing
  * 0) При запуске процесс в argv[1] принимает номер прослушиваемого порта.
  *    Открывает tcp сокет на указанном порту. Открывает на запись текстовый файл <port_num>.txt
  * 1) Получает на порт <port_num> сообщение от клиента (например, telnet или https://github.com/Bkn58/tcp-tester).
  *    Макс. длина сообщения 128 байт.
  * 2) сохраняет сообщение от клиента в файл, открытый в п.0
  * 3) ожидание 3s
  * 4) возвращает клиенту сообщение “ACCEPTED”
  * 5) При появлении новых данных от клиента действия аналогично пп.2-4
  * 6) При отключении по инициативе клиента сокет закрывается
  * Реализация:
  * 1) Обработка ведется на конкурентной основе - запрос от одного клиента не блокирует запросы от других
  *    2 одновременных запроса от 2 клиентов завершаются за ~3 секунд
  * 2) потоки не используются, для параллельной обработки запросов используется liburing
  * 3) для сборки проекта используется cmake
  *
  * За основу взят пример https://github.com/frevib/io_uring-echo-server
  */
#include <QCoreApplication>
#include <QFile>
#include <QDebug>
#include <QDateTime>
#include <QSettings>
#include "server.h"

#define NAME_LOG_FILE ".log"  // файл логиирования сообщений в каталоге запуска приложения(qDebug ...)
#define INI_FILE      ".conf" // файл настроек (располагается в каталоге запуска приложения)

/**
 * Функция переопределения обработчика событий при выводе отладочных сообщений
 *
 */
void myMessageOutput(QtMsgType type,  const QMessageLogContext &context, const QString &msg)
{
    QFile fMessFile(qApp->applicationFilePath() + NAME_LOG_FILE);
    if(!fMessFile.open(QFile::Append | QFile::Text)){
        return;
    }

    QString sCurrDateTime = "[" +
              QDateTime::currentDateTime().toString("hh:mm:ss.zzz") + "]";
    QTextStream tsTextStream(&fMessFile);
    //context.line
    switch(type){
    case QtDebugMsg:
        tsTextStream << QString("%1 Debug - (%2)%3").arg(sCurrDateTime).arg(context.line).arg(msg);
        break;
    case QtWarningMsg:
        tsTextStream << QString("%1 Warning - %2").arg(sCurrDateTime).arg(msg);
        break;
    case QtCriticalMsg:
        tsTextStream << QString("%1 Critical - %2").arg(sCurrDateTime).arg(msg);
        break;
    case QtInfoMsg:
        tsTextStream << QString("%1 Info - %2").arg(sCurrDateTime).arg(msg);
    case QtFatalMsg:
        tsTextStream << QString("%1 Fatal - %2").arg(sCurrDateTime).arg(msg);
        abort();
    }

    tsTextStream.flush();
    fMessFile.flush();
    fMessFile.close();

}

int main(int argc, char *argv[])
{
    int port_no;
    QCoreApplication a(argc, argv);
    QFile fMessFile(qApp->applicationFilePath() + NAME_LOG_FILE);

    QSettings conf(qApp->applicationFilePath() + INI_FILE,QSettings::IniFormat);

    fMessFile.remove();
    fMessFile.close();
    qInstallMessageHandler(myMessageOutput);

    qDebug () << Q_FUNC_INFO << "conf.fileName()" << conf.fileName() << "\n";
    if (argc > 1) {
        port_no = strtol(argv[1], NULL, 10);
    }
    else {
        printf ("Usage: %s port_number\n",argv[0]);
        qCritical () <<  Q_FUNC_INFO << "Usage: " << argv[0] << " port_number\n";
        qDebug () << Q_FUNC_INFO << "Порт не задан, чтение настроек из файла " << conf.fileName() << "\n";
        // чтение настроек из файла tcpserver.conf
        QString confPort = conf.value("port").toString();
        if (confPort=="") {
            qCritical () <<  Q_FUNC_INFO << "Файл tcpserver.conf не найден. Stop program!\n";
            exit(0);
        }
        else
            port_no = confPort.toInt();
    }
    conf.setValue("port",port_no);
    conf.sync(); // сохранение настроек

    printf ("portno= %d\n",port_no);
    qDebug () << Q_FUNC_INFO << "Port number=" << port_no << "\n";
    qDebug() <<  Q_FUNC_INFO << "Start program!\n";

    server *srv = new server (port_no);
    srv->start();

    return a.exec();
}
