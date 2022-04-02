#include "http_conn.h"




// 定义http响应的状态信息
const char *status_200_title = "OK";
const char *status_400_title = "Bad Request";
const char *status_400_form = "Your request  has a syntax error";
const char *status_403_title = "Forbidden";
const char *status_403_form = "Your request was rejected by the server";
const char *status_404_title = "Not Found";
const char *status_404_form = "The requested resource could not be found on the server";
const char *status_500_title = "Internal Server Error";
const char *status_500_form = "The server encountered an error while executing the request";



