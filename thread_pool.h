#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "locker.h"

//线程池类，定义为模板类，便于代码的复用，模板参数T为任务类
template<typename T>
class threadPool{ 
public:
    threadPool(int threadNum = 8, int maxReqsts = 10000);

    bool append(T* request);

    ~threadPool();

private:
    static void * work(void * arg);
    void run();

private:
    //线程数量
    int m_threadNum;
    
    //线程池数组，大小为m_threadNum
    pthread_t * m_threads;

    //请求队列最多允许的，等待请求的数量
    int m_maxReqsts;

    //请求队列
    std::list<T*> m_workQueue;

    //互斥锁
    locker m_queueLocker;

    //信号量：判断是否有任务要处理
    sem m_queueStat;

    //是否结束线程
    bool m_stop;
};

template<typename T>
threadPool<T>::threadPool(int threadNum, int maxReqsts) :
    m_threadNum(threadNum), m_maxReqsts(maxReqsts), 
    m_stop(false), m_threads(NULL) {

        if(threadNum <= 0 || maxReqsts <= 0){
            throw std::exception();
        }

        m_threads = new pthread_t[m_threadNum];
        if(!m_threads){
            throw std::exception();
        }

        //创建threadNum个线程，并设置为线程脱离
        for(int i = 0; i<threadNum; ++i){
            printf("创建第 %d 个线程\n", i);

            if(pthread_create(m_threads + i, NULL, work, this) != 0){ //work作为静态成员不能访问非静态成员，因此最后一个参数用this
                delete [] m_threads;
                throw std::exception();
            }

            if(pthread_detach(m_threads[i])){
                delete [] m_threads;
                throw std::exception();
            }
        }
    }

template<typename T>
threadPool<T>::~threadPool(){
    delete [] m_threads;
    m_stop = true;
}

template<typename T>
bool threadPool<T>::append(T* request){
    // 操作工作队列时一定要加锁，因为它被所有线程共享
    m_queueLocker.lock();
    //超出最大请求数量，报错
    if(m_workQueue.size() > m_maxReqsts){
        m_queueLocker.unlock();
        return false;
    }

    m_workQueue.push_back(request);
    m_queueLocker.unlock();
    m_queueStat.post();
    return true;
}

template<typename T>
void *threadPool<T>::work(void* arg){
    threadPool * pool = (threadPool *) arg;
    pool->run();
    return pool;
}

template<typename T>
void threadPool<T>::run(){
    
    while(!m_stop){
        m_queueStat.wait();
        m_queueLocker.lock();
        if(m_workQueue.empty()){ //队列满了->退出
            m_queueLocker.unlock();
            continue;
        }

        T* request = m_workQueue.front();
        m_workQueue.pop_front();
        m_queueLocker.unlock();

        if(!request){
            continue;
        }

        request->process();
    }
}
    


#endif