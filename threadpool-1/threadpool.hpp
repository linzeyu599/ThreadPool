#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>

//Any类型：可以接收任意数据的类型
class Any
{
public:
	Any() = default;
	~Any() = default;
	Any(const Any&) = delete;
	Any& operator=(const Any&) = delete;
	Any(Any&&) = default;
	Any& operator=(Any&&) = default;

	//这个构造函数可以让Any类型接收任意其它的数据
	template<typename T>//T:int    Derive<int>
	Any(T data) : base_(std::make_unique<Derive<T>>(data))
	{}

	//这个方法能把Any对象里面存储的data数据提取出来
	template<typename T>
	T cast_()
	{
		//我们怎么从base_找到它所指向的Derive对象，从它里面取出data成员变量
		//基类指针 =》 派生类指针   RTTI
		Derive<T>* pd = dynamic_cast<Derive<T>*>(base_.get());//获取这个智能指针存储的裸指针
		if (pd == nullptr)
		{
			throw "type is unmatch!";//类型不匹配
		}
		return pd->data_;
	}
private:
	//基类类型
	class Base
	{
	public:
		virtual ~Base() = default;//新标准，编译器对代码指令的优化更多一些
	};

	//派生类类型
	template<typename T>
	class Derive : public Base
	{
	public:
		Derive(T data) : data_(data) 
		{}
		T data_;//保存了任意的其它类型
	};

private:
	//定义一个基类的指针
	std::unique_ptr<Base> base_;
};

//实现一个信号量类
class Semaphore
{
public:
	Semaphore(int limit = 0) 
		:resLimit_(limit)
	{}
	~Semaphore() = default;

	//获取一个信号量资源
	void wait()
	{
		std::unique_lock<std::mutex> lock(mtx_);
		//等待信号量有资源，没有资源的话，会阻塞当前线程
		cond_.wait(lock, [&]()->bool {return resLimit_ > 0; });
		resLimit_--;
	}

	//增加一个信号量资源
	void post()
	{
		std::unique_lock<std::mutex> lock(mtx_);
		resLimit_++;
		//linux下condition_variable的析构函数什么也没做
		//导致这里状态已经失效，无故阻塞
		cond_.notify_all();//等待状态，释放mutex锁 通知条件变量wait的地方，可以起来干活了
	}
private:
	int resLimit_;
	std::mutex mtx_;
	std::condition_variable cond_;
};

//Task类型的前置声明
class Task;

//实现接收提交到线程池的task任务执行完成后的返回值类型Result
class Result
{
public:
	Result(std::shared_ptr<Task> task, bool isValid = true);
	~Result() = default;

	//问题一：setVal方法，获取任务执行完的返回值的
	void setVal(Any any);

	//问题二：get方法，用户调用这个方法获取task的返回值
	Any get();
private:
	Any any_;//存储任务的返回值
	Semaphore sem_;//线程通信信号量
	std::shared_ptr<Task> task_;//强智能指针指向对应获取返回值的任务对象 
	std::atomic_bool isValid_;//表示返回值是否有效
};

//任务抽象基类，给用户提供统一的处理方法，让用户可以传入各种类型的任务
class Task
{
public:
	Task();
	~Task() = default;
	void exec();
	void setResult(Result* res);

	//用户可以自定义任意任务类型，从Task继承，重写run方法，实现自定义任务处理
	virtual Any run() = 0;

private:
	Result* result_;//Result对象的生命周期 是大于 Task的，不能用强智能指针，引发交叉引用
};

//线程池支持的模式，C++新标准，访问这个枚举项的时候，要加上类型，杜绝枚举类型不同，枚举项名字相同产生的冲突问题
enum class PoolMode
{
	MODE_FIXED,//固定数量的线程
	MODE_CACHED,//线程数量可动态增长
};

//线程类型
class Thread
{
public:
	//线程函数对象类型
	using ThreadFunc = std::function<void(int)>;

	//线程构造
	Thread(ThreadFunc func);
	//线程析构
	~Thread();
	//启动线程
	void start();

	//获取线程id
	int getId()const;
private:
	ThreadFunc func_;
	static int generateId_;
	int threadId_;//保存线程id
};

/*
example:
ThreadPool pool;
pool.start(4);

class MyTask : public Task
{
	public:
		void run() { // 线程代码... }
};

pool.submitTask(std::make_shared<MyTask>());
*/

//线程池类型
class ThreadPool
{
public:
	//线程池构造
	ThreadPool();

	//线程池析构
	~ThreadPool();

	//设置线程池的工作模式
	void setMode(PoolMode mode);

	//设置task任务队列上线阈值，方便用户修改，因为有的用户的硬件配置高
	void setTaskQueMaxThreshHold(int threshhold);

	//设置线程池cached模式下线程阈值
	void setThreadSizeThreshHold(int threshhold);

	//给线程池提交任务
	Result submitTask(std::shared_ptr<Task> sp);

	//开启线程池
	void start(int initThreadSize = std::thread::hardware_concurrency());

	//禁止用户对线程池对象进行拷贝构造和赋值
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

private:
	//定义线程函数
	void threadFunc(int threadid);

	//检查pool的运行状态
	bool checkRunningState() const;

private:
	//std::vector<std::unique_ptr<Thread>> threads_;//线程列表
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;//线程列表，线程id+线程对象

	int initThreadSize_;//初始的线程数量，可以用CPU的内核数来确定
	int threadSizeThreshHold_;//线程数量上限阈值
	std::atomic_int curThreadSize_;//记录当前线程池里面线程的总数量
	std::atomic_int idleThreadSize_;//记录空闲线程的数量

	std::queue<std::shared_ptr<Task>> taskQue_;//任务队列，1.得保证这个任务对象一直存在，2.通过基类指针指向派生类对象，产生多态
	std::atomic_int taskSize_;//任务的数量，通过CAS保证原子操作
	int taskQueMaxThreshHold_;//任务队列数量上限阈值，不会去修改它，不用考虑线程安全

	std::mutex taskQueMtx_;//保证任务队列的线程安全
	std::condition_variable notFull_;//表示任务队列不满，表示用户线程可以生产任务了
	std::condition_variable notEmpty_;//表示任务队列不空，线程池可以从任务队列取任务了
	std::condition_variable exitCond_;//等到线程资源全部回收

	PoolMode poolMode_;//当前线程池的工作模式
	std::atomic_bool isPoolRunning_;//表示当前线程池的启动状态，因为有可能在多个线程使用到，定义为CAS原子类型
};

#endif