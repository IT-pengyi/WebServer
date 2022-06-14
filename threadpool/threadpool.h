#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list> 
#include <cstdio>
#include <pthread.h>
#include <exception>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"


template <typename T>
class threadpool
{
private:
    //工作线程运行的函数，它不断的从工作队列中取出任务并执行,因ptherad_create第三个参数原因（void*)，设置为静态函数
    static void* worker(void* arg);
    void run();
public:
    //thread_number是线程池中线程的数量(本机为四核处理器)，max_requests是请求队列中最多允许的、等待处理的请求的数量
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T* request, int state);  //往请求队列添加任务
    bool append_p(T* request);

private:
    int m_thread_number;        //线程池中线程数
    int m_max_requests;          //请求队列中允许的最大请求数
    pthread_t* m_threads;       //描述线程池的数组，大小为 m_thread_number
    std::list<T*> m_workqueue;  //请求队列
    locker m_queuelocker;       //互斥锁:保护请求队列
    sem m_queuestat;            //信号量:用来确定是否有任务需要处理
    int m_actor_model;          //模型切换
    connection_pool* m_connPool;    //数据库

};

template<typename T>
threadpool<T>::threadpool(int actor_model, connection_pool* connPool, int thread_number, int max_requests) : m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0) {
        throw std::exception();
    }
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads) {
        throw std::exception();
    }
    //创建thread_number个线程，并将他们设置为脱离线程
    for (int i = 0; i < thread_number; ++i) {
        //循环创建线程，并将工作线程按要求进行运行
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) {
            delete[] m_threads;
            throw std::exception();
        }
        //将线程进行分离后，不用单独对工作线程进行回收
        if (pthread_detach(m_threads[i])) {
            delete [] m_threads;
            throw std::exception();
        }
    }
}

template< typename T>
threadpool<T>::~threadpool() {
    delete [] m_threads;
}

/*往请求队列添加任务*/
template<typename T>
bool threadpool<T>::append_p(T* request) {
    m_queuelocker.lock();            //操作工作队列必须加锁，因为它被所有线程共享
    if (m_workqueue.size() > m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);     //添加任务
    m_queuelocker.unlock();
    m_queuestat.post();         //信号量提醒有任务要处理
    return true;
}

template<typename T>
bool threadpool<T>::append(T* request, int state) {
    m_queuelocker.lock();
    if (m_workqueue.size() > m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;       //
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

template<typename T>
void* threadpool<T>::worker(void* arg) {
    threadpool* pool = (threadpool*)arg;
    pool->run();
    return pool;
}

/*工作线程从请求队列中取出某个任务进行处理，注意线程同步*/
template<typename T>
void threadpool<T>::run() {
    while (true)
    {
        m_queuestat.wait();         //信号量等待
        m_queuelocker.lock();       //被唤醒后先加互斥锁
        if (m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();    //从请求队列中取出第一个任务
        m_workqueue.pop_front();            //将任务从请求队列删除
        m_queuelocker.unlock();
        if (!request) {
            continue;
        }
        if (1 == m_actor_model)
        {
            if (0 == request->m_state)
            {
                if (request->read_once())
                {
                    request->improv = 1;
                    connectionRAII mysqlcon(&request->mysql, m_connPool);       //数据库连接池取和归还
                    request->process();
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
            else
            {
                if (request->write())
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        else
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();              //  process(模板类中的方法,这里是http类)进行处理
        }
        
    }
    
}



#endif