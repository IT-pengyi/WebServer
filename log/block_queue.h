#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include "../lock/locker.h"
using namespace std;

template <typename T>
class block_queue {
public:
    block_queue(int max_size = 1000);
     
    ~block_queue();

    void clear();

    //判断队列是否满了
    bool full();

    //判断队列是否为空
    bool empty();

    //返回队首元素
    bool front(T& value);

     //返回队尾元素
    bool back(T& value);

    int size();

    int max_size();
    

    //往队列添加元素，需要将所有使用队列的线程先唤醒
    bool push(const T& item);

    //pop时,如果当前队列没有元素,将会等待条件变量
    bool pop(T& item);
    
    //超时处理
    bool pop(T& item, int ms_timeout);

private:
    locker m_mutex;
    cond m_cond;

    T* m_array;
    int m_size;
    int m_max_size;
    int m_front;
    int m_back;
};

template <typename T>
block_queue<T>::block_queue(int max_size): m_max_size(max_size) {
    if (max_size <= 0) {
        exit(-1);
    }
    m_array = new T[max_size];
    m_size = 0;
    m_front = -1;
    m_back = -1;
}

template <typename T>
block_queue<T>::~block_queue() {
    m_mutex.lock();
    if (m_array != nullptr) {
        delete [] m_array;
    }
    m_mutex.unlock();
}

template <typename T>
void block_queue<T>::clear() {
    m_mutex.lock();
    m_size = 0;
    m_front = -1;
    m_back = -1;
    m_mutex.unlock();
}

//判断队列是否满了
template <typename T>
bool block_queue<T>::full() {
    m_mutex.lock();
    if (m_size >= m_max_size) {
        m_mutex.unlock();
        return true;
    }
    m_mutex.unlock();
    return false;
}

//判断队列是否为空
template <typename T>
bool block_queue<T>::empty() {
    m_mutex.lock();
    if (m_size == 0) {
        m_mutex.unlock();
        return true;
    }
    m_mutex.unlock();
    return false;
}

//返回队首元素
template <typename T>
bool block_queue<T>::front(T& value) {
    m_mutex.lock();
    if (m_size == 0) {
        m_mutex.unlock();
        return false;
    }
    value = m_array[m_front];
    m_mutex.unlock();
    return true;
}

//返回队尾元素
template <typename T>
bool block_queue<T>::back(T& value) {
    m_mutex.lock();
    if (m_size == 0) {
        m_mutex.unlock();
        return false;
    }
    value = m_array[m_back];
    m_mutex.unlock();
    return true;
}

template <typename T>
int block_queue<T>:: size() {
    int temp = 0;
    m_mutex.lock();
    temp = m_size;
    m_mutex.unlock();
    return temp;
}

template <typename T>
int block_queue<T>:: max_size() {
    int temp = 0;
    m_mutex.lock();
    temp = m_max_size;
    m_mutex.unlock();
    return temp;
}

//往队列添加元素，需要将所有使用队列的线程先唤醒
//当有元素push进队列,相当于生产者生产了一个元素
//若当前没有线程等待条件变量,则唤醒无意义
template <typename T>
bool block_queue<T>::push(const T& item) {
    m_mutex.lock();
    if (m_size >= m_max_size) {
        m_cond.broadcast();
        m_mutex.unlock();
        return false;
    }
    m_back = (m_back + 1) % m_max_size;
    m_array[m_back] = item;
    ++m_size;
    m_cond.broadcast();
    m_mutex.unlock();
    return true;
}

//pop时,如果当前队列没有元素,将会等待条件变量
template <typename T>
bool block_queue<T>:: pop(T& item) {
    m_mutex.lock();
    //多个消费者的时候，这里要是用while而不是if
    while (m_size <= 0) {
        //当重新抢到互斥锁，pthread_cond_wait返回为0
        if (!m_cond.wait(m_mutex.get())) {      
            m_mutex.unlock();
            return false;
        }
    }

    m_front = (m_front + 1) % m_max_size;
    item = m_array[m_front];
    --m_size;
    m_mutex.unlock();
    return true;

}

//超时处理
template <typename T>
bool block_queue<T>::pop(T& item, int ms_timeout) {
    struct timespec t = {0, 0};     //struct timespec有两个成员，一个是秒，一个是纳秒, 所以最高精确度是纳秒。
    struct timeval now = {0, 0};    //struct timeval有两个成员，一个是秒，一个是微秒, 所以最高精确度是微秒。
    gettimeofday(&now, NULL);       
    m_mutex.lock();
    if (m_size <= 0) {
        t.tv_sec = now.tv_sec + ms_timeout / 1000;
        t.tv_nsec = (ms_timeout % 1000) * 1000;
        if (!m_cond.timewait(m_mutex.get(), t)) {
            m_mutex.unlock();
            return false;
        }
    }
    if (m_size <= 0)
    {
        m_mutex.unlock();
        return false;
    }
    m_front = (m_front + 1) % m_max_size;
    item = m_array[m_front];
    --m_size;
    m_mutex.unlock();
    return true;

}

#endif