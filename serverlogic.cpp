#include "serverlogic.h"

#include <QSqlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QJsonArray>
#include <QCryptographicHash>


ServerLogic::ServerLogic(QObject *parent) : QTcpServer(parent)
{
    connect(this, &ServerLogic::newConnection, this, &ServerLogic::onNewConnection);
    database = QSqlDatabase::addDatabase("QSQLITE");
    database.setDatabaseName(QDir::homePath() + "/MessengerData.db");
    if (!database.open())
    {
        qCritical() << "Could not connect to database:" << database.lastError().text();
        exit(1);
    }
    Logger::getInstance()->logToFile("Server is running");
}

//Обработка подключений и запросов от клиентов
void ServerLogic::onNewConnection()
{
    QTcpSocket *clientSocket = this->nextPendingConnection();
    qintptr socketId = clientSocket->socketDescriptor();
    QString logMessage = QString("New connection. Client socket descriptor: %1").arg(socketId);
    Logger::getInstance()->logToFile(logMessage);
    connect(clientSocket, &QTcpSocket::readyRead, this, [this, clientSocket]()
            {
                //Прием данных от клиента
                QByteArray jsonData = clientSocket->readAll();
                QJsonParseError parseError;
                QJsonDocument document = QJsonDocument::fromJson(jsonData, &parseError);

                if (parseError.error != QJsonParseError::NoError)
                {
                    clientSocket->write("{\"status\":\"error\",\"message\":\"Invalid JSON format\"}");
                    return;
                }

                QJsonObject json = document.object();
                qDebug() << "Received JSON:" << json;

                //Обработка запроса на регистрацию
                if (json.contains("type") && json["type"].toString() == "register" &&
                    json.contains("login") && json.contains("password"))
                {
                    QString login = json["login"].toString();
                    QString hashedPassword = json["password"].toString();

                    //Проверка допустимости логина
                    if (loginAvailable(login) && loginContainsOnlyAllowedCharacters(login))
                    {

                        //Добавление пользователя в базу данных
                        QSqlQuery query(database);
                        query.prepare("INSERT INTO user_auth (login, password, nickname) "
                                      "VALUES (:login, :password, :nickname)");
                        query.bindValue(":login", login);
                        query.bindValue(":password", hashedPassword); // Сохраняем полученный от клиента хеш пароля
                        query.bindValue(":nickname", "New user"); // Используем логин в качестве никнейма
                        if (!query.exec())
                        {
                            //Ошибка при добавлении пользователя в БД
                            clientSocket->write("{\"status\":\"error\",\"message\":\"Failed to register user\"}");
                        }
                        else
                        {
                            //Пользователь успешно добавлен в БД
                            clientSocket->write("{\"status\":\"success\",\"message\":\"User registered successfully\"}");
                            Logger::getInstance()->logToFile(QString("User '%1' was successfully registered.").arg(login));
                        }
                    }
                    else
                    {
                        //Информировать клиента о недопустимости логина
                        clientSocket->write("{\"status\":\"error\",\"message\":\"Login validation failed\"}");
                    }
                }
                else if(json.contains("type") && json["type"].toString() == "register")
                {
                    clientSocket->write("{\"status\":\"error\",\"message\":\"Missing required fields\"}");
                }

                else if (json.contains("type") && json["type"].toString() == "login" &&
                         json.contains("login") && json.contains("password"))
                {
                    QString login = json["login"].toString();
                    QString hashedPassword = json["password"].toString();

                    QSqlQuery query(database);
                    query.prepare("SELECT password FROM user_auth WHERE login = :login");
                    query.bindValue(":login", login);
                    query.exec();

                    if (query.next())
                    {
                        QString storedPassword = query.value(0).toString();
                        if(storedPassword == hashedPassword)
                        {
                            //Пароли совпадают, успешный вход
                            clientSocket->write("{\"status\":\"success\",\"message\":\"Logged in successfully\"}");
                            Logger::getInstance()->logToFile(QString("User '%1' logged in successfully.").arg(login));
                            QSqlQuery userIdQuery(database);
                            userIdQuery.prepare("SELECT user_id FROM user_auth WHERE login = :login");
                            userIdQuery.bindValue(":login", login);
                            if (userIdQuery.exec() && userIdQuery.next())
                            {
                                int userId = userIdQuery.value("user_id").toInt();
                                userSockets.insert(userId, clientSocket);
                                qDebug() << "user_id = " << userId;
                                Logger::getInstance()->logToFile(QString("User '%1' with ID '%2' added to userSockets.").arg(login).arg(userId));
                            }
                        }
                        else
                        {
                            //Пароли не совпадают
                            clientSocket->write("{\"status\":\"error\",\"message\":\"Login failed. Incorrect password.\"}");
                        }
                    }
                    else if(json.contains("type") && json["type"].toString() == "login") //???
                    {
                        //Логин не найден в базе данных
                        clientSocket->write("{\"status\":\"error\",\"message\":\"Login failed. User not found.\"}");
                    }
                }
                else if (json.contains("type") && json["type"].toString() == "check_nickname" && json.contains("login"))
                {
                    QString login = json["login"].toString();
                    QSqlQuery query(database);
                    query.prepare("SELECT nickname FROM user_auth WHERE login = :login");
                    query.bindValue(":login", login);
                    if(query.exec() && query.next())
                    {
                        QString nickname = query.value(0).toString();
                        QJsonObject response;
                        response["type"] = "check_nickname";
                        response["status"] = "success";
                        response["nickname"] = nickname;
                        qDebug() << nickname << "\n";
                        //Отправить найденный никнейм обратно клиенту
                        qDebug() << QJsonDocument(response).toJson(QJsonDocument::Compact);
                        clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
                        clientSocket->flush();
                    }
                }
                else if (json.contains("type") && json["type"].toString() == "update_nickname" &&
                         json.contains("login") && json.contains("nickname"))
                {
                    QString login = json["login"].toString();
                    QString nickname = json["nickname"].toString();

                    //Проверка никнейма на допустимость
                    if (!nickname.isEmpty() && nickname != "New user") {
                        QSqlQuery query(database);
                        query.prepare("UPDATE user_auth SET nickname = :nickname WHERE login = :login");
                        query.bindValue(":nickname", nickname);
                        query.bindValue(":login", login);
                        if (!query.exec())
                        {
                            QJsonObject response;
                            response["type"] = "update_nickname";
                            response["status"] = "error";
                            response["message"] = "Не удалось обновить имя.";
                            clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
                        }
                        else
                        {
                            QJsonObject response;
                            response["type"] = "update_nickname";
                            response["status"] = "success";
                            response["message"] = "Nickname has been changed.";
                            QString logMessage = QString("User with login '%1' has changed their name to '%2'").arg(login, nickname);
                            Logger::getInstance()->logToFile(logMessage);
                            clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
                        }
                    }
                    else
                    {
                        QJsonObject response;
                        response["type"] = "update_nickname";
                        response["status"] = "error";
                        response["message"] = "Недопустимое имя.";
                        clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
                    }
                    clientSocket->flush();
                }
                else if (json.contains("type") && json["type"].toString() == "find_users" && json.contains("searchText") && json.contains("login"))
                {
                    handleFindUsers(clientSocket, json);
                }
                else if (json.contains("type") && json["type"].toString() == "update_login" &&
                         json.contains("old_login") && json.contains("new_login") && json.contains("password"))
                {
                    QString oldLogin = json["old_login"].toString();
                    QString newLogin = json["new_login"].toString();
                    QString clientPassword = json["password"].toString();

                    //Проверка допустимости логина и нового логина
                    if (!loginAvailable(newLogin) || !loginContainsOnlyAllowedCharacters(newLogin))
                    {
                        clientSocket->write("{\"type\":\"update_login\", \"status\":\"error\",\"message\":\"Invalid or duplicate new login.\"}");
                        clientSocket->flush();
                        return;
                    }

                    QSqlQuery query(database);

                    //Проверяем существование старого логина и его пароля
                    query.prepare("SELECT password FROM user_auth WHERE login = :oldLogin");
                    query.bindValue(":oldLogin", oldLogin);
                    if (query.exec() && query.next()) {
                        QString dbHashedPassword = query.value(0).toString();

                        //Если пароли совпадают
                        if (getSha256Hash(clientPassword, oldLogin) == dbHashedPassword) {
                            //Зашифровываем пароль с использованием нового логина как соли
                            QString newHashedPassword = getSha256Hash(clientPassword, newLogin);

                            //Обновляем данные пользователя в БД
                            query.prepare("UPDATE user_auth SET login = :newLogin, password = :newHashedPassword WHERE login = :oldLogin");
                            query.bindValue(":newLogin", newLogin);
                            query.bindValue(":newHashedPassword", newHashedPassword);
                            query.bindValue(":oldLogin", oldLogin);

                            if (query.exec())
                            {
                                clientSocket->write("{\"type\":\"update_login\", \"status\":\"success\",\"message\":\"Login and password updated successfully.\"}");
                            }
                            else
                            {
                                clientSocket->write("{\"type\":\"update_login\", \"status\":\"error\",\"message\":\"Could not update login and password in the database.\"}");
                            }
                        }
                        else
                        {
                            clientSocket->write("{\"type\":\"update_login\", \"status\":\"error\",\"message\":\"Incorrect old password.\"}");
                        }
                    }
                    else
                    {
                        clientSocket->write("{\"type\":\"update_login\", \"status\":\"error\",\"message\":\"Old login not found.\"}");
                    }
                    clientSocket->flush();
                }
                else if (json.contains("type") && json["type"].toString() == "update_password" &&
                         json.contains("login") && json.contains("current_password") && json.contains("new_password"))
                {
                    QString login = json["login"].toString();
                    QString currentPassword = json["current_password"].toString(); //Предполагается, что пароль хэшируется на клиенте
                    QString newPassword = json["new_password"].toString(); //Предполагается, что пароль хэшируется на клиенте

                    QSqlQuery query(database);
                    query.prepare("SELECT password FROM user_auth WHERE login = :login");
                    query.bindValue(":login", login);
                    if (query.exec() && query.next()) {
                        QString storedPassword = query.value(0).toString();

                        if (storedPassword == currentPassword)
                        {

                            query.prepare("UPDATE user_auth SET password = :newPassword WHERE login = :login");
                            query.bindValue(":newPassword", newPassword);
                            query.bindValue(":login", login);

                            if (query.exec())
                            {
                                clientSocket->write("{\"type\":\"update_password\", \"status\":\"success\",\"message\":\"Password updated successfully.\"}");
                            }
                            else
                            {
                                clientSocket->write("{\"type\":\"update_password\", \"status\":\"error\",\"message\":\"Could not update password.\"}");
                            }
                        }
                        else
                        {
                            clientSocket->write("{\"type\":\"update_password\", \"status\":\"error\",\"message\":\"Incorrect current password.\"}");
                        }
                    }
                    else
                    {
                        clientSocket->write("{\"type\":\"update_password\", \"status\":\"error\",\"message\":\"Login not found.\"}");
                    }
                    clientSocket->flush();
                }
                else if (json.contains("type") && json["type"].toString() == "create_chat" && json.contains("user1") && json.contains("user2"))
                {
                    handleCreateChat(clientSocket, json);
                }
                else if (json.contains("type") && json["type"].toString() == "get_chat_list" && json.contains("login"))
                {
                    handleGetChatList(clientSocket, json);
                }
                else if (json.contains("type")&& json["type"].toString() == "get_chat_history" && json.contains("chat_id"))
                {
                    handleGetChatHistory(clientSocket, json);
                }
                else if (json.contains("type")&& json["type"].toString() == "send_message" && json.contains("chat_id") &&
                        json.contains("user_id") && json.contains("message_text"))
                {
                    handleSendMessage(clientSocket, json);
                }
                else if (json.contains("type")&& json["type"].toString() == "get_or_create_chat")
                {
                    handleGetOrCreateChat(clientSocket, json);
                }
                else if (json.contains("type") && json["type"].toString() == "delete_chat" && json.contains("chat_id"))
                {
                    handleDeleteChat(clientSocket, json);
                }
            });
}


//Запуск сервера на определенном порте
void ServerLogic::startServer(int port)
{
    if (!this->listen(QHostAddress::Any, port))
    {
        qCritical() << "Could not start server";
    }
    else
    {
        qDebug() << "Server started on port" << port;
    }
}

//Выключение сервера
void ServerLogic::shutdownServer()
{
    this->close();
    Logger::getInstance()->logToFile("Server is turned off");

    //Отключение всех клиентов
    foreach(QTcpSocket *socket, userSockets)
    {
        if (socket->state() == QTcpSocket::ConnectedState)
        {
            socket->disconnectFromHost();
        }
        socket->deleteLater();
    }
    userSockets.clear();

    //Закрыть соединение с базой данных, если открыто
    if (database.isOpen())
    {
        database.close();
    }
}

//Проверка пароля
bool ServerLogic::passwordContainsRequiredCharacters(const QString &password)
{
    QRegularExpression upperCaseRegExp("[A-Z]"); //Регулярное выражение для заглавных букв
    QRegularExpression lowerCaseRegExp("[a-z]"); //Регулярное выражение для строчных букв
    QRegularExpression digitRegExp("\\d");       //Регулярное выражение для цифр
    QRegularExpression specialRegExp("[!@#$%^&*()_+=-]"); //Регулярное выражение для специальных символов

    return password.contains(upperCaseRegExp) &&
           password.contains(lowerCaseRegExp) &&
           password.contains(digitRegExp) &&
           password.contains(specialRegExp);
}

//Проверка логина
bool ServerLogic::loginContainsOnlyAllowedCharacters(const QString &login)
{
    QRegularExpression loginRegExp("^[A-Za-z\\d_-]+$"); //Регулярное выражение для допустимых символов в логине
    return login.contains(loginRegExp);
}

//Проверка доступности логина при регистрации
bool ServerLogic::loginAvailable(const QString& login)
{
    QSqlQuery query(database);
    query.prepare("SELECT COUNT(*) FROM user_auth WHERE login = :login");
    query.bindValue(":login", login);
    query.exec();

    if (query.next() && query.value(0).toInt() == 0)
    {
        return true; //Логин свободен
    }
    return false; //Логин занят
}

//Шифрование пароля
QString ServerLogic::getSha256Hash(const QString &str, const QString &salt)
{
    QByteArray byteArrayPasswordSalt = (str + salt).toUtf8();
    QByteArray hashedPassword = QCryptographicHash::hash(byteArrayPasswordSalt, QCryptographicHash::Sha256).toHex();
    return hashedPassword;
}

//Обработка запроса по поиску пользователей
void ServerLogic::handleFindUsers(QTcpSocket* clientSocket, const QJsonObject &json)
{
    QString searchText = json["searchText"].toString();
    QString userLogin = json["login"].toString();

    QSqlQuery query(database);
    query.prepare("SELECT login, nickname FROM user_auth WHERE nickname LIKE :nickname AND login != :login");
    query.bindValue(":nickname", '%' + searchText + '%');
    query.bindValue(":login", userLogin); //Исключаем пользователя из результатов
    if (!query.exec())
    {
        QJsonObject response;
        response["status"] = "error";
        response["message"] = "Ошибка при поиске пользователей.";
        clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
    }
    else
    {
        QJsonArray usersArray;
        while (query.next())
        {
            QString nickname = query.value("nickname").toString();
            QString login = query.value("login").toString();
            QJsonObject userObj;
            userObj["nickname"] = nickname;
            userObj["login"] = login;
            usersArray.append(userObj);
        }
        QJsonObject response;
        response["status"] = "success";
        response["users"] = usersArray;
        clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
    }
    clientSocket->flush();
}

//Обработка запроса на создание чата
void ServerLogic::handleCreateChat(QTcpSocket* clientSocket, const QJsonObject &json)
{
    QString user1 = json["user1"].toString();
    QString user2 = json["user2"].toString();
    QString chatName = user1 + user2;

    QSqlQuery query(database);

    //Проверяем, существует ли уже такой чат
    query.prepare("SELECT chat_id FROM chats WHERE chat_name = :chatName");
    query.bindValue(":chatName", chatName);
    if (query.exec() && query.next()) {
        // ат уже существует, возвращаем ID чата
        int chatId = query.value("chat_id").toInt();
        QJsonObject response;
        response["type"] = "create_chat";
        response["status"] = "success";
        response["chat_id"] = chatId;
        clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
        clientSocket->flush();
        return;
    }

    //Вставляем новый чат в таблицу chats
    query.prepare("INSERT INTO chats (chat_name, chat_type) VALUES (:chatName, 'personal')");
    query.bindValue(":chatName", chatName);
    if (!query.exec()) {
        QJsonObject response;
        response["type"] = "create_chat";
        response["status"] = "error";
        response["message"] = "Failed to create chat.";
        clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
        clientSocket->flush();
        return;
    }

    //Получаем ID нового чата
    int chatId = query.lastInsertId().toInt();

    //Вставляем участников в таблицу chat_participants
    query.prepare("INSERT INTO chat_participants (chat_id, user_id) "
                  "SELECT :chatId, user_id FROM user_auth WHERE login = :userLogin");
    query.bindValue(":chatId", chatId);
    query.bindValue(":userLogin", user1);
    if (!query.exec())
    {
        QJsonObject response;
        response["type"] = "create_chat";
        response["status"] = "error";
        response["message"] = "Failed to add user1 to chat.";
        clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
        clientSocket->flush();
        return;
    }
    query.bindValue(":chatId", chatId);
    query.bindValue(":userLogin", user2);
    if (!query.exec())
    {
        QJsonObject response;
        response["type"] = "create_chat";
        response["status"] = "error";
        response["message"] = "Failed to add user2 to chat.";
        clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
        clientSocket->flush();
        return;
    }

    //Возвращаем успешный ответ
    QJsonObject response;
    response["type"] = "create_chat";
    response["status"] = "success";
    response["chat_id"] = chatId;
    Logger::getInstance()->logToFile(QString("Chat successfully created and users added to chat ID: %1").arg(chatId));
    clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
    clientSocket->flush();
}

//Обработка запроса на получение списка чатов
void ServerLogic::handleGetChatList(QTcpSocket* clientSocket, const QJsonObject &json)
{
    QString login = json["login"].toString();

    QSqlQuery query(database);
    query.prepare(
        "SELECT c.chat_id, u2.nickname AS other_nickname "
        "FROM chats c "
        "JOIN chat_participants cp1 ON c.chat_id = cp1.chat_id "
        "JOIN user_auth u1 ON cp1.user_id = u1.user_id "
        "JOIN chat_participants cp2 ON c.chat_id = cp2.chat_id AND cp2.user_id != cp1.user_id "
        "JOIN user_auth u2 ON cp2.user_id = u2.user_id "
        "WHERE u1.login = :login AND c.chat_type = 'personal'"
        );
    query.bindValue(":login", login);

    if (!query.exec())
    {
        qCritical() << "Ошибка выполнения SQL запроса: " << query.lastError();
        QJsonObject response;
        response["status"] = "error";
        response["message"] = "Ошибка при получении списка чатов.";
        clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
        clientSocket->flush();
        return;
    }

    QJsonArray chatsArray;
    while (query.next())
    {
        int chatId = query.value("chat_id").toInt();
        QString otherNickname = query.value("other_nickname").toString();

        //Проверяем количество непрочитанных сообщений
        QSqlQuery unreadQuery(database);
        unreadQuery.prepare("SELECT COUNT(*) FROM messages m "
            "LEFT JOIN message_read_status mrs ON m.message_id = mrs.message_id "
            "WHERE m.chat_id = :chatId AND m.user_id != (SELECT user_id FROM user_auth WHERE login = :login) AND mrs.timestamp_read IS NULL");
        unreadQuery.bindValue(":chatId", chatId);
        unreadQuery.bindValue(":login", login);

        if (!unreadQuery.exec() || !unreadQuery.next())
        {
            qCritical() << "Ошибка выполнения SQL запроса для непрочитанных сообщений: " << unreadQuery.lastError();
            continue;
        }

        int unreadCount = unreadQuery.value(0).toInt();

        QJsonObject chatObj;
        chatObj["chat_id"] = chatId;
        chatObj["other_nickname"] = otherNickname;
        chatObj["unread_count"] = unreadCount; //Добавляем информацию о непрочитанных сообщениях

        chatsArray.append(chatObj);
    }

    QJsonObject response;
    response["status"] = "success";
    response["chats"] = chatsArray;
    clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
    clientSocket->flush();
}

//Обработка запроса на отправку сообщения
void ServerLogic::handleSendMessage(QTcpSocket* clientSocket, const QJsonObject &json)
{
    QString chatIdStr = json["chat_id"].toString();
    QString userLogin = json["user_id"].toString(); //Здесь на самом деле передается login пользователя
    QString messageText = json["message_text"].toString();
    QString timestamp = json["timestamp"].toString(); //Получаем временную метку

    int chatId = chatIdStr.toInt();

    //Получаем user_id по логину пользователя
    QSqlQuery userIdQuery(database);
    userIdQuery.prepare("SELECT user_id FROM user_auth WHERE login = :login");
    userIdQuery.bindValue(":login", userLogin);

    if (!userIdQuery.exec() || !userIdQuery.next())
    {
        qCritical() << "Ошибка получения user_id для логина: " << userLogin;
        return;
    }

    int userId = userIdQuery.value("user_id").toInt();

    //Вставляем сообщение в базу данных
    QSqlQuery query(database);
    query.prepare("INSERT INTO messages (chat_id, user_id, message_text, timestamp_sent) "
                  "VALUES (:chatId, :userId, :messageText, :timestamp)");
    query.bindValue(":chatId", chatId);
    query.bindValue(":userId", userId);
    query.bindValue(":messageText", messageText);
    query.bindValue(":timestamp", timestamp); //Привязываем временную метку

    if (!query.exec())
    {
        qCritical() << "Ошибка добавления сообщения: " << query.lastError();
        return;
    }

    QJsonObject response;
    response["type"] = "send_message";
    response["status"] = "success";
    clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
    clientSocket->flush();

    Logger::getInstance()->logToFile(QString("Message sent in chat ID: %1 by user: %2 at %3")
        .arg(chatId).arg(userId).arg(timestamp));

    //Находим второго пользователя в чате
    QSqlQuery participantQuery(database);
    participantQuery.prepare("SELECT user_id FROM chat_participants WHERE chat_id = :chatId AND user_id != :userId");
    participantQuery.bindValue(":chatId", chatId);
    participantQuery.bindValue(":userId", userId);

    if (participantQuery.exec() && participantQuery.next())
    {
        int otherUserId = participantQuery.value("user_id").toInt();
        qDebug() << "Other user id: " << otherUserId;
        //Вернем уведомление второму пользователю, если он онлайн
        if (userSockets.contains(otherUserId)) {
            qDebug() << "Other user socket: " << userSockets[otherUserId];
            QTcpSocket *otherUserSocket = userSockets[otherUserId];
            QJsonObject notification;
            notification["type"] = "chat_update";
            notification["chat_id"] = chatIdStr;
            notification["message_text"] = messageText;
            notification["timestamp"] = timestamp;
            notification["user_id"] = userLogin; //Добавляем login пользователя, отправившего сообщение
            //Отправить новое сообщение в чат пользователя
            otherUserSocket->write(QJsonDocument(notification).toJson(QJsonDocument::Compact));
            otherUserSocket->flush();
        }
    }
}

//Обработка запроса на получение истории чата
void ServerLogic::handleGetChatHistory(QTcpSocket* clientSocket, const QJsonObject &json)
{
    if (!json.contains("chat_id") || !json.contains("login"))
    {
        qCritical() << "Invalid request: missing chat_id or login";
        return;
    }

    QString chatIdStr = json["chat_id"].toString();
    QString login = json["login"].toString();
    int chatId = chatIdStr.toInt();

    //Получаем user_id по login
    QSqlQuery userIdQuery(database);
    userIdQuery.prepare("SELECT user_id FROM user_auth WHERE login = :login");
    userIdQuery.bindValue(":login", login);

    if (!userIdQuery.exec() || !userIdQuery.next())
    {
        qCritical() << "Failed to fetch user_id for login:" << login;
        return;
    }
    int userId = userIdQuery.value("user_id").toInt();

    qDebug() << "User ID from handleGetChatHistory: " << userId;
    qDebug() << "Chat ID from handleGetChatHistory: " << chatId;

    QSqlQuery query(database);
    query.prepare("SELECT ua.login AS user_id, m.message_text, m.timestamp_sent AS timestamp "
                  "FROM messages m "
                  "JOIN user_auth ua ON m.user_id = ua.user_id "
                  "WHERE m.chat_id = :chatId "
                  "ORDER BY m.timestamp_sent");
    query.bindValue(":chatId", chatId);

    if (!query.exec())
    {
        qCritical() << "Error fetching chat history:" << query.lastError();
        return;
    }

    QJsonArray messagesArray;
    while (query.next())
    {
        QString messageUserId = query.value("user_id").toString();
        QString messageText = query.value("message_text").toString();
        QString timestamp = query.value("timestamp").toString();

        QJsonObject messageObj;
        messageObj["user_id"] = messageUserId;
        messageObj["message_text"] = messageText;
        messageObj["timestamp"] = timestamp;

        messagesArray.append(messageObj);
    }

    //Отметить сообщения как прочитанные
    markMessagesAsRead(chatId, userId);

    QJsonObject response;
    response["type"] = "get_chat_history";
    response["messages"] = messagesArray;
    clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
    clientSocket->flush();
}

//Обработка запроса на открытие или создание чата
void ServerLogic::handleGetOrCreateChat(QTcpSocket* clientSocket, const QJsonObject &json)
{
    QString login1 = json["login1"].toString();
    QString login2 = json["login2"].toString();
    QString chatName1 = login1 + login2;
    QString chatName2 = login2 + login1; //Вариант, когда промежуточный chatName другой
    QSqlQuery query(database);

    //Проверяем, существует ли уже такой чат
    query.prepare("SELECT chat_id FROM chats WHERE chat_name = :chatName1 OR chat_name = :chatName2");
    query.bindValue(":chatName1", chatName1);
    query.bindValue(":chatName2", chatName2);

    if (query.exec() && query.next())
    {
        int chatId = query.value("chat_id").toInt();
        QJsonObject response;
        response["type"] = "get_or_create_chat";
        response["status"] = "success";
        response["chat_id"] = QString::number(chatId);  //Преобразование в строку для передачи
        qDebug() << "Existing chatId:" << chatId;
        clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
        clientSocket->flush();
        return;
    }

    //Вставляем новый чат в таблицу chats
    query.prepare("INSERT INTO chats (chat_name, chat_type) VALUES (:chatName1, 'personal')");
    query.bindValue(":chatName1", chatName1);
    if (!query.exec())
    {
        QJsonObject response;
        response["type"] = "get_or_create_chat";
        response["status"] = "error";
        response["message"] = "Failed to create chat.";
        qCritical() << "Failed to create chat:" << query.lastError().text();
        clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
        clientSocket->flush();
        return;
    }

    //Получаем ID нового чата
    int chatId = query.lastInsertId().toInt();
    qDebug() << "New chatId created:" << chatId;

    //Вставляем участников в таблицу chat_participants для обоих логинов
    query.prepare("INSERT INTO chat_participants (chat_id, user_id) "
                  "SELECT :chatId, user_id FROM user_auth WHERE login = :login1 OR login = :login2");
    query.bindValue(":chatId", chatId);
    query.bindValue(":login1", login1);
    query.bindValue(":login2", login2);
    if (!query.exec())
    {
        QJsonObject response;
        response["type"] = "get_or_create_chat";
        response["status"] = "error";
        response["message"] = "Failed to add users to chat.";
        qCritical() << "Failed to add users to chat:" << query.lastError().text();
        clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
        clientSocket->flush();
        return;
    }

    //Возвращаем успешный ответ с chat_id
    QJsonObject response;
    response["type"] = "get_or_create_chat";
    response["status"] = "success";
    response["chat_id"] = QString::number(chatId);
    clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
    clientSocket->flush();
}

//Отметить сообщения как прочитанные
void ServerLogic::markMessagesAsRead(int chatId, int userId)
{
    QSqlQuery selectQuery(database);
    selectQuery.prepare("SELECT message_id FROM messages WHERE chat_id = :chatId AND user_id != :userId");
    selectQuery.bindValue(":chatId", chatId);
    selectQuery.bindValue(":userId", userId);

    if (!selectQuery.exec())
    {
        qCritical() << "Error fetching message IDs to mark as read:" << selectQuery.lastError().text();
        return;
    }

    QSqlQuery insertQuery(database);
    while (selectQuery.next())
    {
        int messageId = selectQuery.value("message_id").toInt();
        insertQuery.prepare("INSERT OR IGNORE INTO message_read_status (message_id, user_id, timestamp_read) "
                            "VALUES (:messageId, :userId, CURRENT_TIMESTAMP)");
        insertQuery.bindValue(":messageId", messageId);
        insertQuery.bindValue(":userId", userId);

        if (!insertQuery.exec())
        {
            qCritical() << "Error marking message as read:" << insertQuery.lastError().text();
        }
        else
        {
            Logger::getInstance()->logToFile(QString("Marked message ID: %1 as read in chat ID: %2 for user ID: %3")
                .arg(messageId).arg(chatId).arg(userId));
        }
    }
}

//Обработка запроса на удаление чата
void ServerLogic::handleDeleteChat(QTcpSocket* clientSocket, const QJsonObject &json)
{
    if (!json.contains("chat_id"))
    {
        qCritical() << "Invalid delete chat request: missing chat_id";
        QJsonObject response;
        response["type"] = "error";
        response["message"] = "Missing chat_id";
        clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
        clientSocket->flush();
        return;
    }

    //Извлекаем chat_id как строку
    QString chatIdStr = json["chat_id"].toString();
    if (chatIdStr.isEmpty())
    {
        qCritical() << "Invalid delete chat request: empty chat_id string";
        QJsonObject response;
        response["type"] = "error";
        response["message"] = "Invalid chat_id";
        clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
        clientSocket->flush();
        return;
    }

    //Преобразуем строку в целое число
    bool ok;
    int chatId = chatIdStr.toInt(&ok);
    if (!ok)
    {
        qCritical() << "Invalid delete chat request: chat_id is not a valid integer";
        QJsonObject response;
        response["type"] = "error";
        response["message"] = "Invalid chat_id";
        clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
        clientSocket->flush();
        return;
    }

    qDebug() << "Deleting chat with ID:" << chatId;

    //Удаление чата из базы данных
    QSqlQuery query(database);
    query.prepare("DELETE FROM chats WHERE chat_id = :chatId");
    query.bindValue(":chatId", chatId);

    if (!query.exec())
    {
        qCritical() << "Error deleting chat: " << query.lastError();
        QJsonObject response;
        response["type"] = "error";
        response["message"] = "Failed to delete chat";
        clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
        clientSocket->flush();
        return;
    }

    QJsonObject response;
    response["type"] = "success";
    response["message"] = "Chat deleted successfully";
    clientSocket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
    clientSocket->flush();

    Logger::getInstance()->logToFile(QString("Chat ID: %1 deleted successfully").arg(chatId));
}






