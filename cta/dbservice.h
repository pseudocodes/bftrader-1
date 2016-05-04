#pragma once

#include <QObject>

namespace leveldb {
class DB;
}

//
// 1. robot-
// 2. order-
// 3. trade-
// 4. model-
//
// DB
class DbService : public QObject {
    Q_OBJECT
public:
    explicit DbService(QObject* parent = 0);
    void init();
    void shutdown();

signals:

public slots:
    void dbOpen();
    void dbClose();

private:
    leveldb::DB* db_ = nullptr;
};