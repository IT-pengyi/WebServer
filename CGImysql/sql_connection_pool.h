#ifndef SQL_CONNECION_POOL_H
#define SQL_CONNECION_POOL_H

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"


class sql_connection_pool
{
private:
    /* data */
public:
    sql_connection_pool(/* args */);
    ~sql_connection_pool();
};

sql_connection_pool::sql_connection_pool(/* args */)
{
}

sql_connection_pool::~sql_connection_pool()
{
}



#endif





