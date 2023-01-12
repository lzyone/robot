// headers in this package
#include "TcpClient.h"
#include "LogUtil.h"
#include "Tool.h"

// headers in STL
#include <vector>
#include <array>
#include <chrono>
#include <pthread.h>
#include <sys/prctl.h>

// headers in boost
#include <boost/foreach.hpp>

using namespace rockbot::network;

TcpClient::TcpClient(std::string ip_address, int port, int timeout)
    : socket_(io_service_)
    , timer_(io_service_)
    , strand_(io_service_)
    , is_connecting_(false)
    , is_canceled_(false)
    , ip_address_(ip_address)
    , port_(port)
    , timeout_(timeout)
    , retry_interval_(kRetryInterval)
{
}

TcpClient::TcpClient(std::string ip_address, int port, std::string channel_name, int interval, int timeout)
    : ClientChannel(interval)
    , socket_(io_service_)
    , timer_(io_service_)
    , strand_(io_service_)
    , is_connecting_(false)
    , is_canceled_(false)
    , ip_address_(ip_address)
    , port_(port)
    , timeout_(kDefaultTimeout)
    , retry_interval_(kRetryInterval)
{
    channel_name_.assign(channel_name, 0, kMaxChannelNamePrefixLen);
    io_thread_name_ = channel_name_ + kChannelNameSuffixCli;
    handle_thread_name_ = channel_name_ + kChannelNameSuffixHandle;
}

TcpClient::~TcpClient() 
{
    //io_service_.stop();
    if (socket_.is_open())
    {
        socket_.close();
    }
    vec_recv_callback_.clear();
    threads_.interrupt_all();
    threads_.join_all();
}

void TcpClient::Start()
{
    Connect();
}

int TcpClient::Connect() {
    if (!is_connecting_) {
        SYS_INFO("Start connect[%s:%d].", ip_address_.c_str(), port_);
        socket_.async_connect(
                boost::asio::ip::tcp::endpoint(boost::asio::ip::address::from_string(ip_address_), port_),
                boost::bind(&TcpClient::OnConnect, GetSharedPtr(), boost::asio::placeholders::error));
        is_connecting_ = true;
    }
    return 0;
}

void TcpClient::OnTimer(const boost::system::error_code &error) {
    if (!error && !is_canceled_) {
        socket_.cancel();
    }
}

// callback function on connect
void TcpClient::OnConnect(const boost::system::error_code &error) {
    SYS_INFO("[%s:%d] on connect.", ip_address_.c_str(), port_);
    is_connecting_ = false;
    if (error) {
        connected_ = false;
        SYS_INFO("[%s:%d] connect error[%s]", ip_address_.c_str(), port_, error.message().c_str());
        if (socket_.is_open())
            socket_.close();
        boost::this_thread::sleep_for(boost::chrono::seconds(kRetryInterval));
        Connect();
        return;
    } else {
        connected_ = true;
        SYS_INFO("[%s:%d] connected ok.", ip_address_.c_str(), port_);
    }
    Receive();
}

void TcpClient::OnReceive(const boost::system::error_code &error, size_t bytes_transferred) {
    char str[kMaxDumpBufLen] = { 0 };
    SYS_TRACE("[%s:%d] recv len[%d] data[%s]", ip_address_.c_str(), port_, bytes_transferred, Tool::MemeryChar(recv_buf_, bytes_transferred, str, sizeof(str)));
    if (error) {
        if (connected_)
            SYS_ERROR("[%s:%d] receive error[%s]", ip_address_.c_str(), port_, error.message().c_str());
        connected_ = false;
        if (socket_.is_open())
            socket_.close();
        boost::this_thread::sleep_for(boost::chrono::seconds(kRetryInterval));
        Connect();
        return;
    }
    std::unique_lock<std::mutex> lock(mtx_recv_);
    if (bytes_transferred > 0)
    {
        for (int i = 0; i < bytes_transferred; i++)
        {
            cache_recv_.push_back(recv_buf_[i]);
        }
        if (cache_recv_.size() >= cache_recv_.capacity())
        {
            SYS_WARN_THROTTLE(LOG_INTERVAL_3S, "[%s:%d] cache buf overflow! capacity[%d]", ip_address_.c_str(), port_, cache_recv_.capacity());
        }
    }
    // notify HandleData
    cv_.notify_one();
    // continue receive
    Receive();
}

void TcpClient::SetRecvCallback(recv_callback_t cb){
    vec_recv_callback_.push_back(cb);
}

void TcpClient::Receive() {
    if(!connected_)
    {
        return;
    }
    //SYS_INFO("start receive.");

    socket_.async_read_some(boost::asio::buffer(recv_buf_),
        strand_.wrap(boost::bind(&TcpClient::OnReceive, GetSharedPtr(), boost::asio::placeholders::error,
                boost::asio::placeholders::bytes_transferred)));
}

void TcpClient::Send(std::string data) {
    SYS_TRACE("[%s:%d] send str[%s]", ip_address_.c_str(), port_, data.c_str());
    if(!connected_)
    {
        SYS_WARN("[%s:%d] The connection is not established, msg not send!", ip_address_.c_str(), port_);
        return;
    }
    if (interval_ > 0)
    {
        SendToCache(data);
        timer_.expires_at(timer_.expires_at() + std::chrono::milliseconds(interval_));
        timer_.async_wait(boost::bind(&TcpClient::HandleSendCache, GetSharedPtr()));
    }
    else
    {
        boost::asio::async_write(socket_, boost::asio::buffer(data),
            strand_.wrap(boost::bind(&TcpClient::OnSend, GetSharedPtr(), boost::asio::placeholders::error,
                boost::asio::placeholders::bytes_transferred)));
    }
    return;
}

void TcpClient::Send(uint8_t* buf, uint32_t len)
{
    if(!connected_)
    {
        SYS_WARN("[%s:%d] The connection is not established, msg not send!", ip_address_.c_str(), port_);
        return;
    }
    if (!buf)
    {
        SYS_WARN("[%s:%d] msg is null!", ip_address_.c_str(), port_);
        return;
    }
    char str[kMaxDumpBufLen] = { 0 };
    SYS_TRACE("[%s:%d] send len[%d] data[%s]", ip_address_.c_str(), port_, len, Tool::MemeryChar(buf, len, str, sizeof(str)));

    if (interval_ > 0)
    {
        SendToCache(buf, len);
        timer_.expires_at(timer_.expires_at() + std::chrono::milliseconds(interval_));
        timer_.async_wait(boost::bind(&TcpClient::HandleSendCache, GetSharedPtr()));
    }
    else
    {
        boost::asio::async_write(socket_, boost::asio::buffer(buf, len),
            strand_.wrap(boost::bind(&TcpClient::OnSend, GetSharedPtr(), boost::asio::placeholders::error,
                boost::asio::placeholders::bytes_transferred)));
    }
}

// callback function for sending data
void TcpClient::OnSend(const boost::system::error_code &error, size_t bytes_transferred) {
    if (error) {
        if (connected_)
            SYS_ERROR("[%s:%d] send error[%s]", ip_address_.c_str(), port_, error.message().c_str());
        else
            SYS_TRACE("[%s:%d] send error[%s]", ip_address_.c_str(), port_, error.message().c_str());

        connected_ = false;
        socket_.close();
        boost::this_thread::sleep_for(boost::chrono::seconds(kRetryInterval));
        Connect();
    } else {
        SYS_TRACE("[%s:%d] send ok. %d", ip_address_.c_str(), port_, bytes_transferred);
    }
}

void rockbot::network::TcpClient::OnSend(const boost::system::error_code& error, std::vector<uint8_t>* data, size_t bytes_transferred)
{
    if (error) {
        if (connected_)
            SYS_ERROR("[%s:%d] send error[%s]", ip_address_.c_str(), port_, error.message().c_str());
        else
            SYS_TRACE("[%s:%d] send error[%s]", ip_address_.c_str(), port_, error.message().c_str());
        connected_ = false;
        socket_.close();
        boost::this_thread::sleep_for(boost::chrono::seconds(kRetryInterval));
        Connect();
    }
    else {
        SYS_TRACE("[%s:%d] send ok. %d", ip_address_.c_str(), port_, bytes_transferred);
    }
    if (data)
    {
        delete data;
        data = 0;
    }
}

// 处理数据
void TcpClient::HandleData()
{
    int ret = 0, total_len = 0, left_len = 0;
    BYTE buf[kMaxRecvBufLen];
#ifdef LINUX
    if (!channel_name_.empty())
        prctl(PR_SET_NAME, handle_thread_name_.c_str());
#endif
    while (!quit_flag_)
    {
        boost::this_thread::interruption_point();
        std::unique_lock<std::mutex> lock(mtx_recv_);
        if (cv_.wait_for(lock, std::chrono::milliseconds(1000)) == std::cv_status::timeout)
            continue;
        //cv_.wait(lock);
        total_len = cache_recv_.size();

        // 待处理数据长度不足
        if (total_len <= 0 || total_len <= left_len)
        {
            boost::this_thread::sleep_for(boost::chrono::microseconds(100));
            continue;
        }

        left_len = total_len = total_len > sizeof(buf) ? sizeof(buf) : total_len;
        memset(buf, 0x00, sizeof(buf));
        //memcpy(buf, &cache_recv_[0], total_len);
        for (int i = 0; i < cache_recv_.size(); i++)
        {
            buf[i] = cache_recv_[i];
        }
        try
        {
            if (!vec_recv_callback_.empty())
            {
                // 遍历回调函数
                for (std::vector<recv_callback_t>::iterator iter = vec_recv_callback_.begin(); iter != vec_recv_callback_.end(); iter++)
                {
                    int len = total_len;
                    if (*iter)
                    {
                        (*iter)(buf, len, NULL);
                        if (len < left_len) // 剩余长度取最大解析后长度
                            left_len = len;
                    }
                    else
                    {
                        SYS_WARN("Erase invalid callback!");
                        vec_recv_callback_.erase(iter);
                    }
                }
            }
            else
            {
                left_len = 0; // 清空缓存
                SYS_WARN("[%s:%d] receive untreated msg: %s", ip_address_.c_str(), port_, recv_buf_);
            }
        }
        catch (std::exception& e)
        {
            left_len = 0; // 清空缓存
            SYS_ERROR("[%s:%d] recv callback error[%s]", ip_address_.c_str(), port_, e.what());
        }
        if (left_len < 0)
        {
            SYS_WARN("[%s:%d] Parse frame error! erase cache!!! left[%d]", ip_address_.c_str(), port_, left_len);
            left_len = 0;
        }
        cache_recv_.erase_begin(total_len-left_len);
    }
    SYS_WARN("[%s]线程退出", handle_thread_name_.c_str());
}

void TcpClient::HandleSendCache()
{
    std::lock_guard<std::mutex> lock(mtx_send_);
    if (cache_send_.empty())
        return;
    if (cache_send_.size() > (timeout_ * 1000 / interval_))
    {
        SYS_WARN("!!!发送缓存数据太多[%d],清空缓存", cache_send_.size());
        cache_send_.clear();
        return;
    }
    while (cache_send_.size() > 0)
    {
        std::vector<uint8_t>* vec = cache_send_.front();
        boost::asio::async_write(socket_, boost::asio::buffer(*vec),
            strand_.wrap(boost::bind(&TcpClient::OnSend, GetSharedPtr(),
                boost::asio::placeholders::error,
                vec,
                boost::asio::placeholders::bytes_transferred)));
        cache_send_.pop_front();
        timer_.expires_from_now(std::chrono::milliseconds(interval_));
        timer_.wait();
    }
}

void TcpClient::ReOpen()
{
    Close();
}

void TcpClient::Close()
{
    if (socket_.is_open())
    {
        socket_.cancel();
        socket_.close();
        connected_ = false;
    }
}

void TcpClient::Destroy()
{
    quit_flag_ = true;
    Close();
    io_service_.stop();
    threads_.interrupt_all();
    threads_.join_all();
}

void TcpClient::SetChannelName(std::string channel_name)
{
    channel_name_.assign(channel_name, 0, kMaxChannelNamePrefixLen);
    io_thread_name_ = channel_name_ + kChannelNameSuffixCli;
    handle_thread_name_ = channel_name_ + kChannelNameSuffixHandle;
}

