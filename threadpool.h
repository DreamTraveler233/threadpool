#ifndef WEBSERVER_THREADPOOL_H
#define WEBSERVER_THREADPOOL_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>

// 线程池支持的模式
enum class PoolMode {
    MODE_FIXED, // 固定数量的线程
    MODE_CACHED,// 线程数量可动态增长
};

// Any类型：可以接收任意类型数据
class Any
{
public:
    Any() = default;
    ~Any() = default;
    Any(const Any &) = delete;
    Any &operator=(const Any &) = delete;
    Any(Any &&) = default;
    Any &operator=(Any &&) = default;

    // 这个构造函数可以让Any类型接收任意其他类型数据
    template<typename T>
    Any(T data) : base_(std::make_unique<Derive<T>>(data))
    {}

    // 将Any对象中的data数据提取出来
    template<typename T>
    T cast()
    {
        // 从base_中找到它所指向的Derive对象，从它里面提取出data成员变量
        // 基类指针 -> 派生类指针
        Derive<T> *pd = dynamic_cast<Derive<T> *>(base_.get());
        if (pd == nullptr)
        {
            throw std::runtime_error("type is unmatch!");
        }
        return pd->data_;
    }

private:
    // 基类类型
    class Base
    {
    public:
        virtual ~Base() = default;// 虚析构函数确保派生类对象能被正确销毁
    };

    // 派生类类型
    template<typename T>
    class Derive : public Base
    {
    public:
        Derive(T data) : data_(data) {}
        T data_;// 保存了具体类型
    };

private:
    // 定义一个基类指针
    std::unique_ptr<Base> base_;
};

// 实现一个信号量类
class Semaphore
{
public:
    Semaphore(int limit = 0) : resLimit_(limit)
    {
    }
    ~Semaphore() = default;

    // 获取一个信号量资源
    void wait()
    {
        std::unique_lock<std::mutex> lock(mtx_);
        // 等待信号量有资源，如果没有则阻塞
        cond_.wait(lock, [this]() { return resLimit_ > 0; });
        resLimit_--;
    }

    // 增加一个信号量资源
    void post()
    {
        std::unique_lock<std::mutex> lock(mtx_);
        resLimit_++;
        cond_.notify_all();
    }

private:
    std::mutex mtx_;
    std::condition_variable cond_;

    int resLimit_;
};

// 声明任务类
class Task;

// 实现接收提交到线程池的task任务执行完成后的返回值类型Result
class Result
{
public:
    Result(const std::shared_ptr<Task> &task, bool isValid_ = true);
    ~Result() = default;

    // 问题一：setValue方法，获取任务执行完的返回值
    void setValue(Any any);

    // 问题二：get方法，获取task任务的返回值
    Any get();

private:
    Any any_;                   // 存储任务返回值
    Semaphore sem_;             // 线程通信信号量
    std::shared_ptr<Task> task_;// 指向对应任务返回值的任务对象
    std::atomic_bool isValid_;  // 返回值是否有效
};

// 任务抽象基类
class Task
{
public:
    Task();
    ~Task() = default;
    void exec();
    void setResult(Result *result);
    // 用户可以自定义任意任务类型，从Task继承，重写run方法，实现自定义任务处理
    virtual Any run() = 0;// 纯虚函数，子类必须实现

private:
    Result *result_;// 不能双方都用智能指针，否则会出现互相引用，无法析构，导致内存泄漏
};

// 线程类型
class Thread
{
public:
    // 线程函数对象类型
    using ThreadFunc = std::function<void(size_t)>;

    // 构造函数
    explicit Thread(ThreadFunc func);
    // 析构函数
    ~Thread() = default;
    // 启动线程
    void start();
    // 获取线程ID
    size_t getId() const;

private:
    ThreadFunc threadFunc_;
    static size_t generateId_;
    size_t threadId_;// 保存线程ID
};

/**
 * 线程池使用方法：
 * 1. 创建线程池对象
 *   ThreadPool pool;
 *
 * 2. 设置线程池模式（可选）
 *   线程池支持两种模式：
 *   - MODE_FIXED：固定数量的线程。
 *   - MODE_CACHED：线程数量可动态增长。
 *   通过 setMode 方法设置模式：
 *   pool.setMode(PoolMode::MODE_FIXED); // 或 PoolMode::MODE_CACHED
 *
 * 3. 设置任务队列上限（可选）
 *   通过 setTaskQueMaxSize 方法设置任务队列的最大容量：
 *   pool.setTaskQueMaxSize(100); // 设置任务队列最大容量为 100
 *
 * 4. 启动线程池
 *   通过 start 方法启动线程池，默认初始化 4 个线程：
 *   pool.start(); // 使用默认线程数
 *   或指定初始线程数：
 *   pool.start(8); // 初始化 8 个线程
 *
 * 5. 定义任务类
 *   任务类需要继承自 Task 基类，并实现 run 方法：
 *   class MyTask : public Task {
 *   public:
 *       void run() override {
 *           // 任务具体逻辑
 *       }
 *   };
 *
 * 6. 提交任务
 *   通过 submitTask 方法提交任务：
 *   auto task = std::make_shared<MyTask>();
 *   pool.submitTask(task);
 */
// 线程池类型
class ThreadPool
{
public:
    // 线程池构造函数
    ThreadPool();

    // 线程池析构函数
    ~ThreadPool();

    // 设置线程池工作模式
    void setMode(PoolMode mode);

    // 设置task任务队列上限数量
    void setTaskQueMaxSize(size_t size);

    // 设置线程池cached模式下线程上限数量
    void setThreadMaxSize(size_t size);

    // 提交任务
    Result submitTask(const std::shared_ptr<Task> &task);

    // 开启线程池
    void start(size_t initThreadSize = std::thread::hardware_concurrency());

    // 禁止拷贝构造和赋值操作
    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

private:
    // 线程函数
    void threadFunc(size_t threadId);

    // 判断线程池运行状态
    bool checkRunningState() const;

private:
    /*====================线程相关变量====================*/
    std::unordered_map<size_t, std::unique_ptr<Thread>> threads_;// 线程列表
    size_t initThreadSize_;                                      // 初始化线程数量
    size_t threadMaxSize_;                                       // 线程上限数量
    std::atomic_uint idleThreadSize_;                            // 空闲线程
    std::atomic_uint currentThreadSize_;                         // 当前线程数量

    /*====================任务相关变量====================*/
    std::queue<std::shared_ptr<Task>> taskQue_;// 任务队列
    size_t taskQueMaxSize_;                    // 任务队列数量上限
    std::atomic_uint currentTaskSize_;         // 当前任务数量

    /*====================线程通信相关变量====================*/
    std::mutex taskQueMtx_;                  // 任务队列互斥锁
    std::condition_variable taskQueNotFull_; // 任务队列非满条件变量
    std::condition_variable taskQueNotEmpty_;// 任务队列非空条件变量
    std::condition_variable exitCond_;       // 线程资源回收条件变量

    /*====================线程池属性相关变量====================*/
    PoolMode poolMode_;             // 当前线程池的工作模式
    std::atomic_bool poolIsRunning_;// 线程池启动标志
};

#endif//WEBSERVER_THREADPOOL_H
