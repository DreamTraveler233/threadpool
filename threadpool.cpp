//
// Created by shuzeyong on 2025/4/14.
//

#include "threadpool.h"

#include <utility>

const size_t TASK_QUE_MAX_SIZE_ = 1024;
const size_t THREAD_MAX_SIZE = 1024;
const size_t THREAD_MAX_IDLE_TIME = 10;// 单位/秒

size_t Thread::generateId_ = 0;

/*====================线程池类方法实现====================*/
// 构造函数
ThreadPool::ThreadPool()
    : initThreadSize_(0),
      currentThreadSize_(0),
      threadMaxSize_(THREAD_MAX_SIZE),
      idleThreadSize_(0),
      currentTaskSize_(0),
      taskQueMaxSize_(TASK_QUE_MAX_SIZE_),
      poolMode_(PoolMode::MODE_CACHED),
      poolIsRunning_(false)

{
}

// 析构函数
ThreadPool::~ThreadPool()
{
    // 修改相关成员变量
    poolIsRunning_ = false;

    // 等待线程池中所以线程返回，线程有两种状态：阻塞（空闲） & 正在执行任务
    std::unique_lock<std::mutex> lock(taskQueMtx_);

    // 唤醒所有阻塞线程
    taskQueNotEmpty_.notify_all();

    exitCond_.wait(lock, [this]() { return threads_.empty(); });
}

// 设置线程池工作模式
void ThreadPool::setMode(PoolMode mode)
{
    if (checkRunningState())// 如果线程池已经启动，则不予设置
    {
        return;
    }
    poolMode_ = mode;
}

// 设置任务队列最大任务数量
void ThreadPool::setTaskQueMaxSize(size_t size)
{
    if (checkRunningState())// 如果线程池已经启动，则不予设置
    {
        return;
    }
    taskQueMaxSize_ = size;
}

// 设置线程池cached模式下线程上限数量
void ThreadPool::setThreadMaxSize(size_t size)
{
    if (checkRunningState())// 如果线程池已经启动，则不予设置
    {
        return;
    }
    if (poolMode_ == PoolMode::MODE_CACHED)
    {
        threadMaxSize_ = size;
    }
}

/**
 * @brief 提交任务到线程池
 *
 * 用户调用该接口，传入任务对象，线程池将任务放入任务队列中等待执行。
 * 如果任务队列已满，则等待1秒，若1秒后仍无法放入任务，则返回失败。
 * 在CACHED模式下，如果任务数量超过空闲线程数量且当前线程数量未达到上限，则会创建新的线程。
 *
 * @param task 要提交的任务对象，类型为std::shared_ptr<Task>
 * @return Result 返回任务的结果对象，包含任务指针和提交是否成功的信息
 */
Result ThreadPool::submitTask(const std::shared_ptr<Task> &task)
{
    // 获取锁，确保对任务队列的访问是线程安全的
    std::unique_lock<std::mutex> lock(taskQueMtx_);

    // 等待任务队列有空余位置，最长等待时间为1秒
    // 如果1秒后任务队列仍然满，则返回提交失败
    if (!taskQueNotFull_.wait_for(lock, std::chrono::seconds(1),
                                  [this]() { return taskQue_.size() < taskQueMaxSize_; }))
    {
        std::cerr << "task queue is full, submit task fail" << std::endl;
        return {task, false};
    }

    // 将任务放入任务队列，并更新当前任务数量
    taskQue_.emplace(task);
    currentTaskSize_++;

    // 通知所有等待的线程，任务队列中有新任务可以处理
    taskQueNotEmpty_.notify_all();

    // 在CACHED模式下，如果任务数量超过空闲线程数量且当前线程数量未达到上限，则创建新线程
    if (poolMode_ == PoolMode::MODE_CACHED    // 处于CACHED模式下
        && currentTaskSize_ > idleThreadSize_ // 当前任务数量大于空闲线程数量
        && currentThreadSize_ < threadMaxSize_// 当前线程数量小于线程上限数量
    )
    {
        // 创建新的线程对象，并启动线程
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        size_t threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr));
        threads_[threadId]->start();
        // 更新线程相关成员变量
        currentThreadSize_++;
        idleThreadSize_++;
        std::cout << "新线程已创建" << std::endl;
    }

    // 返回任务的Result对象，表示任务提交成功
    return {task};
}

// 启动线程池：创建线程，启动线程
void ThreadPool::start(size_t initThreadSize)
{
    // 设置线程池的运行状态为 true，表示线程池已启动
    poolIsRunning_ = true;

    // 初始化线程数量，设置初始线程数和当前线程数
    initThreadSize_ = initThreadSize;
    currentThreadSize_ = initThreadSize;

    // 创建线程对象，每个线程对象绑定到线程池的线程函数
    for (int i = 0; i < initThreadSize_; ++i)
    {
        // 使用 std::bind 将线程函数绑定到线程对象，并使用 std::make_unique 创建线程对象
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));

        // 将线程对象移动到线程池的线程容器中，使用线程 ID 作为键
        this->threads_.emplace(ptr->getId(), std::move(ptr));
    }

    // 启动所有线程，并记录初始空闲线程数量
    for (int i = 0; i < initThreadSize_; ++i)
    {
        threads_[i]->start();// 启动线程，使其开始执行线程函数
        idleThreadSize_++;   // 增加空闲线程计数
    }
}

/**
 * @brief 线程池中的线程函数，负责从任务队列中取出任务并执行。
 *
 * @param threadId 当前线程的ID，用于标识线程池中的线程。
 *
 * 该函数是线程池中每个线程的执行函数。线程会不断从任务队列中取出任务并执行，
 * 直到线程池停止运行。在CACHED模式下，当线程阻塞时，每秒都会检查线程是否需要回收，
 * 当线程空闲时间超过60秒且当前线程数超过初始线程数则会将会被回收。当线程池需要销毁时，
 * 会回收空闲线程和忙线程（等待线程任务完成），每回收一个线程都会通知析构函数检查线程是否全部退出，
 * 如果线程已经全部退出，则将会退出主线程，完成线程池销毁。
 */
void ThreadPool::threadFunc(size_t threadId)
{
    // 记录线程第一次执行任务的时间
    auto lastTime = std::chrono::high_resolution_clock::now();

    // 线程循环工作，不断从任务队列中取出任务并执行
    while (true)
    {
        // 用于临时存储从任务队列中取出的任务
        std::shared_ptr<Task> task;
        {
            // 获取锁以访问任务队列
            std::cout << "线程：" << std::this_thread::get_id() << "尝试获取锁——————————" << std::endl;
            std::unique_lock<std::mutex> lock(taskQueMtx_);

            // 如果任务队列为空，线程进入等待状态
            while (taskQue_.empty())
            {
                // 如果没有任务了，则判断线程池是否需要销毁：
                // 否 -> 继续阻塞
                // 是 -> 回收当前线程
                if (!poolIsRunning_)
                {
                    // 当线程池停止运行时，回收正在执行任务的线程
                    threads_.erase(threadId);
                    std::cout << "线程" << std::this_thread::get_id() << "完成任务，线程已经退出+++++++" << std::endl;
                    // 每回收一个线程唤醒一次主线程，让其重新判断线程是否全部退出完毕：
                    // 是 -> 停止阻塞，线程池销毁完毕
                    // 否 -> 继续阻塞，等待所有线程回收
                    exitCond_.notify_all();
                    return;// 结束线程函数，即结束当前线程
                }

                // 在CACHED模式下，等待任务队列非空并每隔1s检查线程空闲时间是否超过60s
                if (poolMode_ == PoolMode::MODE_CACHED)
                {
                    // 等待任务队列非空，超时时间为1秒，如果超时则需要判断是否需要回收线程
                    if (std::cv_status::timeout == taskQueNotEmpty_.wait_for(lock, std::chrono::seconds(1)))
                    {
                        auto now = std::chrono::high_resolution_clock::now();// 当前时间
                        // 计算线程空闲时间
                        auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                        // 如果线程空闲时间超过60秒且当前线程数超过初始线程数，则回收该线程
                        if (dur.count() >= THREAD_MAX_IDLE_TIME && currentThreadSize_ > initThreadSize_)
                        {
                            // 回收线程
                            threads_.erase(threadId);
                            // 更新线程池相关变量
                            currentThreadSize_--;
                            idleThreadSize_--;
                            std::cout << "线程" << std::this_thread::get_id() << "已销毁" << std::endl;
                            return;
                        }
                    }
                }
                else
                {
                    // 在非CACHED模式下，等待任务队列非空
                    taskQueNotEmpty_.wait(lock);
                }
            }

            // 从任务队列中取出任务
            task = taskQue_.front();
            taskQue_.pop();
            currentTaskSize_--;

            std::cout << "线程：" << std::this_thread::get_id() << "获取任务成功=====" << std::endl;

            // 如果任务队列中还有任务，通知其他线程
            if (!taskQue_.empty())
            {
                taskQueNotEmpty_.notify_all();
            }

            // 通知生产者，可以继续往任务队列放任务
            taskQueNotFull_.notify_all();
        }// 释放锁，允许其他线程访问任务队列

        // 执行任务前，空闲线程数量减少
        idleThreadSize_--;

        // 执行任务
        if (task != nullptr)//保守判断
        {
            task->exec();
        }

        // 任务完成，空闲线程数量增加
        idleThreadSize_++;

        // 更新线程执行完任务的时间
        lastTime = std::chrono::high_resolution_clock::now();
    }
}

bool ThreadPool::checkRunningState() const
{
    return poolIsRunning_;
}

/*====================线程类方法实现====================*/
// 启动线程
void Thread::start()
{
    // 创建一个线程，执行线程函数
    std::thread thread(threadFunc_, threadId_);
    thread.detach();// 设置线程为分离状态，当线程函数执行完毕时，内核自动回收资源，防止线程成为孤儿线程
}

// 构造函数
Thread::Thread(ThreadFunc func)
    : threadFunc_(std::move(func)),
      threadId_(generateId_++)
{}

size_t Thread::getId() const
{
    return threadId_;
}
/*====================任务类方法实现====================*/
Task::Task()
    : result_(nullptr)
{}

void Task::exec()
{
    if (result_ != nullptr)
    {
        result_->setValue(run());// 发生多态，并且将返回值存到any中
    }
}

void Task::setResult(Result *result)
{
    result_ = result;
}


/*====================返回值类方法实现====================*/
// 构造函数
Result::Result(const std::shared_ptr<Task> &task, bool isValid)
    : isValid_(isValid),
      task_(task)
{

    task->setResult(this);
}

// 设置任务返回值
void Result::setValue(Any any)
{
    // 存储task的返回值
    any_ = std::move(any);
    sem_.post();// 表示任务已经完成，可以获取任务返回值
}

// 获取任务返回值
Any Result::get()
{
    if (!isValid_)
    {
        return {};
    }

    sem_.wait();// task任务如果没有执行完，则会阻塞
    return std::move(any_);
}