// headers in this package
#include "AsyncSerial.h"
#include "LogUtil.h"
#include "Tool.h"

// headers in STL
#include <vector>
#include <array>
#include <chrono>
#include <pthread.h>
#include <sys/prctl.h>

using namespace rockbot::network;

AsyncSerial::AsyncSerial(std::string dev_name, uint32_t baud_rate,
    boost::asio::serial_port_base::parity opt_parity,
    boost::asio::serial_port_base::character_size opt_csize,
    boost::asio::serial_port_base::flow_control opt_flow,
    boost::asio::serial_port_base::stop_bits opt_stop)
    : sp_(io_service_)
    , timer_(io_service_)
    , strand_(io_service_)
    , dev_name_(dev_name)
    , is_connecting_(false)
    , is_canceled_(false)
    , timeout_(kDefaultTimeout)
    , retry_interval_(kRetryInterval)
    , baud_rate_(baud_rate)
    , opt_parity_(opt_parity)
    , opt_csize_(opt_csize)
    , opt_flow_(opt_flow)
    , opt_stop_(opt_stop)
{
}

AsyncSerial::AsyncSerial(std::string dev_name, std::string channel_name, uint32_t baud_rate, uint32_t interval,
    boost::asio::serial_port_base::parity opt_parity,
    boost::asio::serial_port_base::character_size opt_csize,
    boost::asio::serial_port_base::flow_control opt_flow,
    boost::asio::serial_port_base::stop_bits opt_stop)
    : ClientChannel(interval)
    , sp_(io_service_)
    , timer_(io_service_)
    , strand_(io_service_)
    , dev_name_(dev_name)
    , is_connecting_(false)
    , is_canceled_(false)
    , timeout_(kDefaultTimeout)
    , retry_interval_(kRetryInterval)
    , baud_rate_(baud_rate)
    , opt_parity_(opt_parity)
    , opt_csize_(opt_csize)
    , opt_flow_(opt_flow)
    , opt_stop_(opt_stop)
{
    channel_name_.assign(channel_name, 0, kMaxChannelNamePrefixLen);
    io_thread_name_ = channel_name_ + kChannelNameSuffixCom;
    handle_thread_name_ = channel_name_ + kChannelNameSuffixHandle;
}

void AsyncSerial::Start()
{
    Open();
}

int AsyncSerial::Open()
{
    SYS_INFO("Start open serial port[%s]", dev_name_.c_str());
    sp_.open(dev_name_, ec_);
    if (ec_)
    {
        timer_.expires_from_now(std::chrono::seconds(timeout_));
        timer_.async_wait(boost::bind(&AsyncSerial::Open, GetSharedPtr()));
        SYS_ERROR("open serial port[%s] error[%s]", dev_name_.c_str(), ec_.message().c_str());
        return -1;
    }
    sp_.set_option(baud_rate_);
    sp_.set_option(opt_flow_);
    sp_.set_option(opt_parity_);
    sp_.set_option(opt_stop_);
    sp_.set_option(opt_csize_);
    connected_ = true;
    SYS_INFO("connected serial port[%s]", dev_name_.c_str());
    Receive();
    return 0;
}

AsyncSerial::~AsyncSerial()
{
    //io_service_.stop();
    if (sp_.is_open())
    {
        sp_.close();
    }
    vec_recv_callback_.clear();
    threads_.interrupt_all();
    threads_.join_all();
}

void AsyncSerial::OnTimer(const boost::system::error_code &error) {
    if (!error && !is_canceled_) {
        sp_.cancel();
    }
}

void AsyncSerial::OnReceive(const boost::system::error_code &error, size_t bytes_transferred) {
    char str[kMaxDumpBufLen] = { 0 };
    SYS_TRACE("[%s] recv len[%d] data[%s]", dev_name_.c_str(), bytes_transferred, Tool::MemeryChar(recv_buf_, bytes_transferred, str, sizeof(str)));
    if (error) {
        if (connected_)
            SYS_ERROR("[%s] receive error : %s", dev_name_.c_str(), error.message().c_str());
        connected_ = false;
        sp_.close();
        Open();
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
            SYS_WARN_THROTTLE(LOG_INTERVAL_3S, "[%s] cache buf overflow! capacity[%d]", dev_name_.c_str(), cache_recv_.capacity());
        }
    }
    // notify HandleData
    cv_.notify_one();
    // continue receive
    Receive();
}

void AsyncSerial::SetRecvCallback(recv_callback_t cb){
    vec_recv_callback_.push_back(cb);
}

void AsyncSerial::Receive() {
    if(!connected_) {
        return;
    }
    SYS_TRACE("[%s] start receive.", dev_name_.c_str());

    sp_.async_read_some(boost::asio::buffer(recv_buf_),
        strand_.wrap(boost::bind(&AsyncSerial::OnReceive, GetSharedPtr(), boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred)));
}

void AsyncSerial::SetChannelName(std::string channel_name)
{
    channel_name_.assign(channel_name, 0, kMaxChannelNamePrefixLen);
    io_thread_name_ = channel_name_ + kChannelNameSuffixCom;
    handle_thread_name_ = channel_name_ + kChannelNameSuffixHandle;
}

void AsyncSerial::HandleSendCache()
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
        boost::asio::async_write(sp_, boost::asio::buffer(*vec),
            strand_.wrap(boost::bind(&AsyncSerial::OnSend, GetSharedPtr(),
                boost::asio::placeholders::error,
                vec,
                boost::asio::placeholders::bytes_transferred)));
        cache_send_.pop_front();
        timer_.expires_from_now(std::chrono::milliseconds(interval_));
        timer_.wait();
    }
}

void AsyncSerial::Send(std::string data) {
    if (!connected_) {
        SYS_WARN("[%s] The connection is not established, msg not send!", dev_name_.c_str());
        return;
    }
    SYS_TRACE("[%s] send str[%s]", dev_name_.c_str(), data.c_str());
    if (interval_ > 0)
    {
        SendToCache(data);
        timer_.expires_at(timer_.expires_at() + std::chrono::milliseconds(interval_));
        timer_.async_wait(boost::bind(&AsyncSerial::HandleSendCache, GetSharedPtr()));
    }
    else
    {
        boost::asio::async_write(sp_, boost::asio::buffer(data),
            strand_.wrap(boost::bind(&AsyncSerial::OnSend, GetSharedPtr(),
                boost::asio::placeholders::error,
                boost::asio::placeholders::bytes_transferred)));
    }
}

void AsyncSerial::Send(uint8_t* buf, uint32_t len) {
    if(!connected_) {
        SYS_WARN("[%s] The connection is not established, msg not send!", dev_name_.c_str());
        return;
    }
    char str[kMaxDumpBufLen] = { 0 };
    SYS_TRACE("[%s] send len[%d] data[%s]", dev_name_.c_str(), len, Tool::MemeryChar(buf, len, str, sizeof(str)));
    if (interval_ > 0)
    {
        SendToCache(buf, len);
        timer_.expires_at(timer_.expires_at() + std::chrono::milliseconds(interval_));
        timer_.async_wait(boost::bind(&AsyncSerial::HandleSendCache, GetSharedPtr()));
    }
    else
    {
        boost::asio::async_write(sp_, boost::asio::buffer(buf, len),
            strand_.wrap(boost::bind(&AsyncSerial::OnSend, GetSharedPtr(),
                boost::asio::placeholders::error,
                boost::asio::placeholders::bytes_transferred)));
    }
}

// callback function for sending data
void AsyncSerial::OnSend(const boost::system::error_code &error, size_t bytes_transferred) {
    if (error) {
        if (connected_)
            SYS_ERROR("[%s] send error[%s]", dev_name_.c_str(), error.message().c_str());
        connected_ = false;
        Open();
    } else {
        SYS_DEBUG("[%s] send ok. %d", dev_name_.c_str(), bytes_transferred);
    }
}

void rockbot::network::AsyncSerial::OnSend(const boost::system::error_code& error, std::vector<uint8_t>* data, size_t bytes_transferred)
{
    if (error) {
        if (connected_)
            SYS_ERROR("[%s] send error[%s]", dev_name_.c_str(), error.message().c_str());
        connected_ = false;
        Open();
    }
    else {
        SYS_DEBUG("[%s] send ok. %d", dev_name_.c_str(), bytes_transferred);
    }
    if (data)
    {
        delete data;
        data = 0;
    }
}

// 处理数据
void AsyncSerial::HandleData()
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
                SYS_WARN("[%s] receive untreated msg[%s]", dev_name_.c_str(), recv_buf_);
            }
        }
        catch (std::exception& e)
        {
            left_len = 0; // 清空缓存
            SYS_ERROR("[%s] call recv callback error[%s]", dev_name_.c_str(), e.what());
        }
        if (left_len < 0)
        {
            SYS_WARN("[%s] Parse frame error! erase cache!!! left[%d], ", dev_name_.c_str(), left_len);
            left_len = 0;
        }

        cache_recv_.erase_begin(total_len - left_len);
    }
    SYS_WARN("[%s]线程退出", handle_thread_name_.c_str());
}

void AsyncSerial::ReOpen()
{
    Close();
}

void AsyncSerial::Close()
{
    if (sp_.is_open())
    {
        sp_.cancel();
        sp_.close();
        connected_ = false;
    }
}

void AsyncSerial::Destroy()
{
    quit_flag_ = true;
    io_service_.stop();
    Close();
    threads_.interrupt_all();
    threads_.join_all();
}

