#ifndef LOCKER_H
#define LOCKER_H

#include <pthread.h>
#include <exception>
#include <semaphore.h>

class sem   //信号量
{  
private:
    sem_t m_sem;
public:

    sem () {
        if (sem_init(&m_sem, 0, 0) != 0) {
            throw std::exception();
        }
    }

    sem (int value) {
        if (sem_init(&m_sem, 0, value) != 0) {
            throw std::exception();
        }
    }

    ~sem(){
        if (sem_destroy(&m_sem) != 0) {
            throw std::exception();
        }
    }

    bool wait() {     // -1
        return sem_wait(&m_sem) == 0;
    }

    bool post() {     // +1
        return sem_post(&m_sem) == 0;
    }
};

class locker  //互斥锁
{
private:
    pthread_mutex_t m_mutex;
public:
    locker() {
        if (pthread_mutex_init(&m_mutex, NULL) != 0) {
            throw std::exception();
        }
    }
    
    ~locker() {
        if (pthread_mutex_destroy(&m_mutex) != 0) {
            throw std::exception();
        }
    }

    bool lock() {
        return pthread_mutex_lock(&m_mutex) == 0;
    }

    bool unlock() {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    pthread_mutex_t *get() {
        return &m_mutex;
    }
};

class cond
{
private:
    pthread_cond_t m_cond;
public:
    cond() {
        if (pthread_cond_init(&m_cond, NULL) != 0) {
            throw std::exception();
        } 
    }

    ~cond() {
        if (pthread_cond_destroy(&m_cond) != 0) {
            throw std::exception();
        }
    }

    bool wait(pthread_mutex_t* m_mutex) {
        return pthread_cond_wait(&m_cond, m_mutex) == 0;
    }

    bool timedwait(pthread_mutex_t* m_mutex, struct timespec* t) {
        return pthread_cond_timedwait(&m_cond, m_mutex, t) == 0;
    }

    bool signal() {
        return pthread_cond_signal(&m_cond) == 0;
    }

    bool broadcast() {
        return pthread_cond_broadcast(&m_cond) == 0;
    }
};


#endif