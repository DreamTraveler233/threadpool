# 线程池项目说明

## 项目简介

这是一个基于C++11标准实现的通用线程池库，提供了高效的任务调度和线程管理功能。该线程池支持两种工作模式（固定线程数量和动态线程数量），具有任务队列管理、线程自动回收、资源安全释放等特性，适合需要并发处理大量任务的场景。

## 主要特性

- 支持固定线程模式(MODE_FIXED)和动态线程模式(MODE_CACHED)
- 自动管理线程生命周期，避免频繁创建销毁线程的开销
- 线程空闲超时自动回收机制
- 任务队列大小可配置
- 线程安全的任务提交和结果获取
- 优雅的线程池关闭机制

## 使用方法

### 1. 基本使用流程

```cpp
// 1. 创建线程池对象
ThreadPool pool;

// 2. 设置线程池模式（可选，默认为固定模式）
pool.setMode(PoolMode::MODE_CACHED);

// 3. 设置任务队列最大大小（可选，默认为1024）
pool.setTaskQueMaxSize(2048);

// 4. 设置线程池最大线程数（可选，默认为1024）
pool.setThreadMaxSize(512);

// 5. 启动线程池，默认线程数为系统支持的并发线程数
pool.start(4); // 启动4个线程

// 6. 提交任务到线程池
auto result = pool.submitTask([](int a, int b) {
    return a + b;
}, 10, 20);

// 7. 获取任务执行结果（可选）
int sum = result.get(); // 获取计算结果30

// 8. 线程池会在析构时自动回收所有线程资源
```

### 2. 线程池配置选项

#### 设置线程池模式

```cpp
// 固定线程模式（线程数量不变）
pool.setMode(PoolMode::MODE_FIXED);

// 动态线程模式（根据任务数量动态调整线程数量）
pool.setMode(PoolMode::MODE_CACHED);
```

#### 设置任务队列大小

```cpp
// 设置任务队列最大容量为2048
pool.setTaskQueMaxSize(2048);
```

#### 设置最大线程数（仅CACHED模式有效）

```cpp
// 设置最大线程数为512
pool.setThreadMaxSize(512);
```

### 3. 提交任务

线程池支持提交任意可调用对象（函数、lambda表达式、函数对象等），并支持获取返回结果。

#### 提交无返回值任务

```cpp
pool.submitTask([]() {
    std::cout << "Hello from thread pool!" << std::endl;
});
```

#### 提交有返回值任务

```cpp
auto future = pool.submitTask([](int a, int b) {
    return a * b;
}, 5, 6);

int result = future.get(); // 获取结果30
```

#### 处理任务队列满的情况

当任务队列已满时，submitTask会等待1秒，如果仍然无法提交任务，则返回一个空的future对象：

```cpp
auto result = pool.submitTask(/* some task */);
if (!result.valid()) {
    // 任务提交失败处理
}
```

### 4. 线程池启动与关闭

#### 启动线程池

```cpp
// 使用默认线程数（硬件并发数）
pool.start();

// 指定初始线程数
pool.start(8); // 启动8个线程
```

#### 关闭线程池

线程池会在析构时自动关闭并回收所有资源，无需手动关闭。

### 5. 完整示例

```cpp
#include "thread_pool.h"
#include <iostream>

int main() {
    // 创建线程池
    ThreadPool pool;
    
    // 配置线程池
    pool.setMode(PoolMode::MODE_CACHED);
    pool.setTaskQueMaxSize(100);
    pool.setThreadMaxSize(20);
    
    // 启动线程池
    pool.start(4);
    
    // 提交多个任务
    std::vector<std::future<int>> results;
    for (int i = 0; i < 10; ++i) {
        results.emplace_back(pool.submitTask([i]() {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            return i * i;
        }));
    }
    
    // 获取结果
    for (auto& result : results) {
        std::cout << "Result: " << result.get() << std::endl;
    }
    
    return 0;
}
```

## 注意事项

1. 所有配置选项（模式、队列大小、线程数量）必须在调用start()之前设置
2. 线程池对象不可拷贝或赋值
3. 在CACHED模式下，空闲线程超过60秒会被自动回收
4. 任务队列满时提交任务可能会失败，需要检查future对象是否有效

## 项目依赖

- C++11或更高版本
- 标准库头文件：<thread>, <mutex>, <condition_variable>, <future>, <atomic>, <functional>等

这个线程池实现简洁高效，适合需要并发处理任务的C++项目使用。
