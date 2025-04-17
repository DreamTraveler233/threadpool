#ifndef THREADPOOL_THREAD_POOL_H
#define THREADPOOL_THREAD_POOL_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>

const size_t TASK_QUE_MAX_SIZE_ = 1024;
const size_t THREAD_MAX_SIZE = 1024;
const size_t THREAD_MAX_IDLE_TIME = 60;// 单位/秒

// 线程池支持的模式
enum class PoolMode {
    MODE_FIXED, // 固定数量的线程
    MODE_CACHED,// 线程数量可动态增长
};

// 线程类
class Thread {
public:
    // 线程函数对象类型
    using ThreadFunc = std::function<void(size_t)>;

    // 构造函数
    inline explicit Thread(ThreadFunc func)
            : threadFunc_(std::move(func)),
              threadId_(generateId_++) {}

    // 析构函数
    inline ~Thread() = default;

    // 启动线程
    inline void start() {
        // 创建一个线程，执行线程函数
        std::thread thread(threadFunc_, threadId_);
        // 设置线程为分离状态，当线程函数执行完毕时，内核自动回收资源，防止线程成为孤儿线程
        thread.detach();
    }

    // 获取线程ID
    inline size_t getThreadId() const {
        return threadId_;
    }

private:
    ThreadFunc threadFunc_;
    static std::atomic_uint generateId_;
    size_t threadId_;// 保存线程ID
};

std::atomic_uint Thread::generateId_ = 0;

/**
 * 1. 创建线程池对象
 * ThreadPool pool;
 * 2. 设置线程池模式（可选，默认为固定模式）
 * pool.setMode(PoolMode::MODE_CACHED);
 * 3. 设置任务队列最大大小（可选，默认为1024）
 * pool.setTaskQueMaxSize(2048);
 * 4. 设置线程池最大线程数（可选，默认为1024）
 * pool.setThreadMaxSize(512);
 * 5. 启动线程池，默认线程数为系统支持的并发线程数
 * pool.start();
 * 6. 提交任务到线程池
 * auto result = pool.submitTask(Func, Args...);
 * 7. 获取任务执行结果（可选）
 * int sum = result.get();
 * 8. 线程池会在析构时自动回收所有线程资源
 */
// 线程池类
class ThreadPool {
public:
    // 线程池构造函数
    inline ThreadPool()
            : initThreadSize_(0),
              currentThreadSize_(0),
              threadMaxSize_(THREAD_MAX_SIZE),
              idleThreadSize_(0),
              currentTaskSize_(0),
              taskQueMaxSize_(TASK_QUE_MAX_SIZE_),
              poolMode_(PoolMode::MODE_FIXED),
              poolIsRunning_(false) {}

    // 线程池析构函数
    inline ~ThreadPool() {
        poolIsRunning_ = false;
        std::unique_lock <std::mutex> lock(taskQueMtx_);
        // 唤醒所有因任务队列为空而阻塞的线程
        taskQueNotEmpty_.notify_all();
        // 等待所有线程退出，直到线程列表为空
        exitCond_.wait(lock, [this]() { return threads_.empty(); });
    }

    // 设置线程池工作模式
    void setMode(PoolMode mode) {
        // 如果线程池已经启动，则不予设置
        if (checkRunningState()) {
            return;
        }
        poolMode_ = mode;
    }

    // 设置task任务队列上限数量
    void setTaskQueMaxSize(size_t size) {
        // 如果线程池已经启动，则不予设置
        if (checkRunningState()) {
            return;
        }
        taskQueMaxSize_ = size;
    }

    // 设置线程池cached模式下线程上限数量
    void setThreadMaxSize(size_t size) {
        // 如果线程池已经启动，则不予设置
        if (checkRunningState()) {
            return;
        }
        if (poolMode_ == PoolMode::MODE_CACHED) {
            threadMaxSize_ = size;
        }
    }

    /**
 * @brief 提交任务到线程池
 *
 * 该函数使用可变参模板编程，允许接受任意任务函数和任意数量的参数。任务会被打包并放入任务队列中，
 * 返回一个std::future对象，用于获取任务的执行结果。
 *
 * @tparam Func 任务函数的类型
 * @tparam Args 任务函数参数的类型
 * @param func 要执行的任务函数
 * @param args 任务函数的参数
 * @return std::future<decltype(func(args...))> 返回一个std::future对象，用于获取任务的执行结果
 */
    template<typename Func, typename... Args>
    auto submitTask(Func &&func, Args &&...args) -> std::future<decltype(func(args...))> {
        // 使用std::packaged_task将任务函数和参数打包，并获取std::future对象
        using RType = decltype(func(args...));
        auto task = std::make_shared < std::packaged_task < RType() >> (
                std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
        std::future <RType> result = task->get_future();

        // 获取锁，确保对任务队列的访问是线程安全的
        std::unique_lock <std::mutex> lock(taskQueMtx_);

        // 等待任务队列有空余位置，最长等待时间为1秒
        // 如果1秒后任务队列仍然满，则返回提交失败
        if (!taskQueNotFull_.wait_for(lock, std::chrono::seconds(1),
                                      [this]() { return taskQue_.size() < taskQueMaxSize_; })) {
            std::cerr << "task queue is full, submit task fail" << std::endl;

            // 返回一个空的std::future对象，表示任务提交失败
            auto tempTask = std::make_shared < std::packaged_task < RType() >> (
                    []() -> RType { return RType(); });
            (*tempTask)();
            return tempTask->get_future();
        }

        // 将任务放入任务队列，并更新当前任务数量
        taskQue_.emplace([task]() { (*task)(); });
        currentTaskSize_++;

        // 通知所有等待的线程，任务队列中有新任务可以处理
        taskQueNotEmpty_.notify_all();

        // 在CACHED模式下，如果任务数量超过空闲线程数量且当前线程数量未达到上限，则创建新线程
        if (poolMode_ == PoolMode::MODE_CACHED && currentTaskSize_ > idleThreadSize_ &&
            currentThreadSize_ < threadMaxSize_) {
            try {
                // 创建新的线程对象，并启动线程
                auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
                size_t threadId = ptr->getThreadId();
                threads_.emplace(threadId, std::move(ptr));
                threads_[threadId]->start();
                // 更新线程相关成员变量
                currentThreadSize_++;
                idleThreadSize_++;
            } catch (const std::system_error &e) {
                // 可以根据实际情况进行其他处理，比如记录日志、返回错误信息等
                std::cerr << "Failed to create a new thread: " << e.what() << std::endl;
            }
        }

        // 返回任务的std::future对象，表示任务提交成功
        return result;
    }

    /**
 * @brief 启动线程池，初始化并启动指定数量的线程。
 *
 * 该函数用于启动线程池，初始化线程数量，并启动所有线程。线程池的运行状态会被设置为 true，
 * 表示线程池已启动。线程数量可以通过参数指定，默认值为系统支持的并发线程数。
 *
 * @param initThreadSize 初始线程数量，默认为系统支持的并发线程数。
 */
    void start(size_t initThreadSize = std::thread::hardware_concurrency()) {
        // 若线程池已启动，直接返回
        if (poolIsRunning_) {
            return;
        }

        // 设置线程池的运行状态为 true，表示线程池已启动
        poolIsRunning_ = true;

        // 初始化线程数量，设置初始线程数和当前线程数
        initThreadSize_ = initThreadSize;
        currentThreadSize_ = initThreadSize;

        // 创建线程对象，每个线程对象绑定到线程池的线程函数
        for (int i = 0; i < initThreadSize_; ++i) {
            try {
                // 使用 std::bind 将线程函数绑定到线程对象，并使用 std::make_unique 创建线程对象
                auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));

                // 将线程对象移动到线程池的线程容器中，使用线程 ID 作为键
                this->threads_.emplace(ptr->getThreadId(), std::move(ptr));
            } catch (const std::system_error &e) {
                // 可以根据实际情况进行其他处理，比如减少初始线程数量、记录日志等
                std::cerr << "Failed to create a new thread during startup: " << e.what() << std::endl;
            }
        }

        // 启动所有线程，并记录初始空闲线程数量
        for (int i = 0; i < initThreadSize_; ++i) {
            threads_[i]->start();// 启动线程，使其开始执行线程函数
            idleThreadSize_++;   // 增加空闲线程计数
        }
    }

    // 禁止拷贝构造和赋值操作
    ThreadPool(const ThreadPool &) = delete;

    ThreadPool &operator=(const ThreadPool &) = delete;

private:
    /**
 * @brief 线程函数，用于从任务队列中取出任务并执行。
 *
 * @param threadId 当前线程的唯一标识符，用于在线程池中标识该线程。
 *
 * @details 该函数是线程池中每个线程的执行函数。线程会不断从任务队列中取出任务并执行。
 * 如果任务队列为空，线程会根据线程池的运行模式和状态决定是否进入等待状态或回收线程。
 * 在CACHED模式下，线程会在空闲时间超过指定阈值时被回收。
 * 线程在执行任务时会更新空闲线程数量，并在任务完成后更新最后执行任务的时间。
 */
    void threadFunc(size_t threadId) {
        // 记录线程第一次执行任务的时间
        auto lastTime = std::chrono::high_resolution_clock::now();

        // 线程循环工作，不断从任务队列中取出任务并执行
        while (true) {
            // 用于临时存储从任务队列中取出的任务
            Task task;
            {
                // 获取锁以访问任务队列
                std::unique_lock <std::mutex> lock(taskQueMtx_);

                // 如果任务队列为空，线程进入等待状态
                while (taskQue_.empty()) {
                    // 如果没有任务了，则判断线程池是否需要销毁：
                    // 否 -> 继续阻塞
                    // 是 -> 回收当前线程
                    if (!poolIsRunning_) {
                        // 当线程池停止运行时，回收正在执行任务的线程
                        threads_.erase(threadId);
                        // 每回收一个线程唤醒一次主线程，让其重新判断线程是否全部退出完毕：
                        // 是 -> 停止阻塞，线程池销毁完毕
                        // 否 -> 继续阻塞，等待所有线程回收
                        exitCond_.notify_all();
                        return;// 结束线程函数，即结束当前线程
                    }

                    // 在CACHED模式下，等待任务队列非空并每隔1s检查线程空闲时间是否超过60s
                    if (poolMode_ == PoolMode::MODE_CACHED) {
                        // 等待任务队列非空，超时时间为1秒，如果超时则需要判断是否需要回收线程
                        if (std::cv_status::timeout == taskQueNotEmpty_.wait_for(lock, std::chrono::seconds(1))) {
                            auto now = std::chrono::high_resolution_clock::now();// 当前时间
                            // 计算线程空闲时间
                            auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                            // 如果线程空闲时间超过60秒且当前线程数超过初始线程数，则回收该线程
                            if (dur.count() >= THREAD_MAX_IDLE_TIME && currentThreadSize_ > initThreadSize_) {
                                // 回收线程
                                threads_.erase(threadId);
                                // 更新线程池相关变量
                                currentThreadSize_--;
                                idleThreadSize_--;
                                return;
                            }
                        }
                    } else {
                        // 在非CACHED模式下，等待任务队列非空
                        taskQueNotEmpty_.wait(lock);
                    }
                }

                // 从任务队列中取出任务
                task = taskQue_.front();
                taskQue_.pop();
                currentTaskSize_--;

                // 如果任务队列中还有任务，通知其他线程
                if (!taskQue_.empty()) {
                    taskQueNotEmpty_.notify_all();
                }

                // 通知生产者，可以继续往任务队列放任务
                taskQueNotFull_.notify_all();
            }// 释放锁，允许其他线程访问任务队列

            // 执行任务前，空闲线程数量减少
            idleThreadSize_--;

            // 执行任务
            if (task)//保守判断
            {
                task();
            }

            // 任务完成，空闲线程数量增加
            idleThreadSize_++;

            // 更新线程执行完任务的时间
            lastTime = std::chrono::high_resolution_clock::now();
        }
    }

    // 判断线程池运行状态
    bool checkRunningState() const {
        return poolIsRunning_;
    }

private:
    /*====================线程相关变量====================*/
    std::unordered_map <size_t, std::unique_ptr<Thread>> threads_;// 线程列表
    size_t initThreadSize_;                                      // 初始化线程数量
    size_t threadMaxSize_;                                       // 线程上限数量
    std::atomic_uint idleThreadSize_;                            // 空闲线程
    std::atomic_uint currentThreadSize_;                         // 当前线程数量

    /*====================任务相关变量====================*/
    using Task = std::function<void()>;
    std::queue <Task> taskQue_;        // 任务队列
    size_t taskQueMaxSize_;           // 任务队列数量上限
    std::atomic_uint currentTaskSize_;// 当前任务数量

    /*====================线程通信相关变量====================*/
    std::mutex taskQueMtx_;                  // 任务队列互斥锁
    std::condition_variable taskQueNotFull_; // 任务队列非满条件变量
    std::condition_variable taskQueNotEmpty_;// 任务队列非空条件变量
    std::condition_variable exitCond_;       // 线程资源回收条件变量

    /*====================线程池属性相关变量====================*/
    PoolMode poolMode_;             // 当前线程池的工作模式
    std::atomic_bool poolIsRunning_;// 线程池启动标志
};

#endif//THREADPOOL_THREAD_POOL_H
