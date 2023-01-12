#ifndef __ROCKBOT_COMM_CHANNEL_H__
#define __ROCKBOT_COMM_CHANNEL_H__

// headers in this package
#include "robot_config.h"
#include "LogUtil.h"

// headers in STL
#include <string>
#include <deque>
#include <pthread.h>
#include <sys/prctl.h>
#include <mutex>
#include <condition_variable>

// headers in boost
#include <boost/asio.hpp>
#include <boost/function.hpp>
#include <boost/bind.hpp>
#include <boost/thread.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/circular_buffer.hpp> 

namespace rockbot
{
namespace network
{
    constexpr uint32_t  kMaxChannelNamePrefixLen = 5;
    const char* const kChannelNameSuffixCom = "Com";
    const char* const kChannelNameSuffixCli = "Cli";
    const char* const kChannelNameSuffixSrv = "Srv";
    const char* const kChannelNameSuffixHandle = "Hdl";
class CommChannel : public std::enable_shared_from_this<CommChannel>
{
public:
    CommChannel()
        : work_(io_service_)
        , channel_name_{0}
        , interval_(0)
        , quit_flag_(false)
        , io_thread_name_{0}
        , handle_thread_name_{0} {};
    CommChannel(int interval)
        : work_(io_service_)
        , channel_name_{0}
        , interval_(interval)
        , quit_flag_(false)
        , io_thread_name_{0}
        , handle_thread_name_{0} {};
    virtual ~CommChannel() {};
    // 初始化
    virtual void Init() {
        threads_.create_thread(boost::bind(&CommChannel::HandleData, shared_from_this()));
        threads_.create_thread(boost::bind(&CommChannel::WorkThread, shared_from_this()));
    }
    // 开始
    virtual void Start() = 0;
    // 发送字符串
    virtual void Send(std::string data) = 0;
    // 发送字符数组
    virtual void Send(uint8_t* buf, uint32_t len) = 0;
    // 开始接收
    virtual void Receive() = 0;
    // 关闭
    virtual void Close() = 0;
    // 销毁
    virtual void Destroy() = 0;
    // 处理数据
    virtual void HandleData() = 0;
    // 获取io service
    virtual boost::asio::io_service* GetIoService() { return &io_service_; };
    std::shared_ptr<CommChannel> GetSharedPtr() {
        return shared_from_this();
    }
    // 设置通道名称
    virtual void SetChannelName(std::string name) {
        channel_name_.assign(name);
    }
    // io service 运行线程
    virtual void WorkThread() {
#ifdef LINUX
        if (!channel_name_.empty())
            prctl(PR_SET_NAME, io_thread_name_.c_str());
#endif
        while (!quit_flag_)
        {
            boost::this_thread::interruption_point();
            Start();
            try
            {
                boost::system::error_code ec;
                io_service_.run(ec);
                if (ec)
                {
                    SYS_ERROR("io_service run error: %s", ec.message().c_str());
                }
            }
            catch (std::runtime_error& ex)
            {
                SYS_ERROR("io_service run exception: %s", ex.what());
            }
            boost::this_thread::sleep(boost::posix_time::seconds(2));
        }
        SYS_WARN("[%s]线程退出", io_thread_name_.c_str());
    }

public:
    // io service 
    boost::asio::io_service io_service_;
    // io  service work
    boost::asio::io_service::work work_;
    // thread group
    boost::thread_group threads_;
    // channel name
    std::string channel_name_;
    std::string io_thread_name_;
    std::string handle_thread_name_;
    int interval_; // 读写间隔时间
    bool quit_flag_; // 退出标志

protected:
    std::mutex mtx_recv_;
    std::mutex mtx_send_;
    std::condition_variable cv_;

};

class ClientChannel : public CommChannel
{
public:
    // 接收回调类型
    typedef boost::function<void(BYTE*, int&, LPVOID pParam)> recv_callback_t;
    ClientChannel()
        : connected_(false)
        , cache_recv_(kMaxCacheBufLen)
        , cache_send_(kMaxCacheSize)
    {
        cache_recv_.clear();
        cache_send_.clear();
    };
    ClientChannel(int interval)
        : CommChannel(interval)
        , connected_(false)
        , cache_recv_(kMaxCacheBufLen)
        , cache_send_(kMaxCacheSize)
    {
        cache_recv_.clear();
        cache_send_.clear();
    };
    virtual ~ClientChannel()
    {
        std::lock_guard<std::mutex> lockr(mtx_recv_);
        cache_recv_.clear();
        std::lock_guard<std::mutex> locks(mtx_send_);
        while (cache_send_.front())
        {
            delete cache_send_.front();
            cache_send_.pop_front();
        }
        cache_send_.clear();
    };
    // 获取连接状态
    inline bool IsConnected() { return connected_; }
    // 设置接收回调
    virtual void SetRecvCallback(recv_callback_t cb) = 0;
    std::shared_ptr<ClientChannel> GetSharedPtr() {
        return std::dynamic_pointer_cast<ClientChannel>(shared_from_this());
    }
    // 发送字符串到缓存
    virtual void SendToCache(std::string data)
    {
        std::lock_guard<std::mutex> lock(mtx_send_);
        std::vector<uint8_t> *vec = new std::vector<uint8_t>(data.begin(), data.end());
        cache_send_.push_back(vec);
    }
    // 发送字符数组到缓存
    virtual void SendToCache(uint8_t* buf, uint32_t len)
    {
        std::lock_guard<std::mutex> lock(mtx_send_);
        std::vector<uint8_t> *vec = new std::vector<uint8_t>(buf, buf+len);
        cache_send_.push_back(vec);
    }
    // 重新打开
    virtual void ReOpen() = 0;
protected:
    // 接收数据缓存
    boost::circular_buffer<uint8_t> cache_recv_;
    // 发送数据缓存
    std::deque<vector<uint8_t>* > cache_send_;
    volatile bool connected_;

};

class ServerChannel : public CommChannel
{
public:
    // 接收回调函数类型
    typedef boost::function<void(std::string, BYTE*, int&, LPVOID pParam)> recv_callback_t;
    //typedef boost::function<void(std::string, uint8_t*, uint32_t)> recv_callback_t;
    // 接收新连接回调函数类型
    typedef boost::function<void(std::string, LPVOID pParam)> accept_callback_t;

    ServerChannel() {};
    ServerChannel(int interval): CommChannel(interval) {};
    virtual ~ServerChannel() {};
    // 设置接收回调
    virtual void SetRecvCallback(recv_callback_t cb) = 0;
    // 设置新连接回调函数
    virtual void SetAcceptCallback(accept_callback_t cb) = 0;
    std::shared_ptr<ServerChannel> GetSharedPtr() {
        return std::dynamic_pointer_cast<ServerChannel>(shared_from_this());
    }
    // 发送字符串
    virtual void Send(std::string data) = 0;
    // 发送字符数组
    virtual void Send(uint8_t* buf, uint32_t len) = 0;
    // 发送字符串
    virtual void Send(std::string sockname, std::string data) = 0;
    // 发送字符数组
    virtual void Send(std::string sockname, uint8_t *buf, uint32_t len) = 0;
protected:
    // 接收buff
    //uint8_t recv_buf_[kMaxRecvBufLen];
    std::map<std::string, boost::circular_buffer<uint8_t> > map_recv_buf_;
};

}
}

#endif //__ROCKBOT_COMM_CHANNEL_H__
