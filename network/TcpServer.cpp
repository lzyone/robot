// headers in this package
#include "TcpServer.h"
#include "LogUtil.h"
#include "Tool.h"

// headers in ROS
//#include <ros/ros.h>

// headers in STL
#include <vector>
#include <array>
#include <chrono>
#include <pthread.h>
#include <sys/prctl.h>

using namespace rockbot::network;

TcpServer::TcpServer(int port)
    : port_(port),
    socket_(io_service_),
    timer_(io_service_),
    strand_(io_service_),
    acceptor_(io_service_, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port))
{
}

TcpServer::TcpServer(int port, std::string channel_name)
    : port_(port),
    socket_(io_service_),
    timer_(io_service_),
    strand_(io_service_),
    acceptor_(io_service_, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port))
{
    channel_name_.assign(channel_name, 0, kMaxChannelNamePrefixLen);
    io_thread_name_ = channel_name_ + kChannelNameSuffixSrv;
    handle_thread_name_ = channel_name_ + kChannelNameSuffixHandle;
}

TcpServer::TcpServer(int port, recv_callback_t cb)
:   port_(port),
    socket_(io_service_),
    timer_(io_service_),
    strand_(io_service_),
    acceptor_(io_service_, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port))
{
    recv_callback_ = cb;
}

TcpServer::~TcpServer() 
{
    //io_service_.stop();
    if (socket_.is_open())
        socket_.close();
    map_clients_.clear();
    threads_.join_all();
}

void TcpServer::Start() {
    SYS_INFO("Start tcp server on port[%d].", port_);
    StartAccept();
}

void TcpServer::StartAccept() {
    SYS_INFO("Start accept connection on port[%d].", port_);
    sock_ptr_t client_ptr_(new boost::asio::ip::tcp::socket(io_service_));
    acceptor_.async_accept(*client_ptr_,
            boost::bind(&TcpServer::OnAccept, GetSharedPtr(), boost::asio::placeholders::error, client_ptr_));
}

void TcpServer::OnAccept(const boost::system::error_code& error, sock_ptr_t sock) {
    try
    {
        StartAccept();
        if (error) {
            SYS_ERROR("accept failed: %s.", error.message().c_str());
            return;
        }
        std::string ipport = GetIPPortFromEndpoint(sock);
        SYS_INFO("accepted new client.(%s)", ipport.c_str());
        AddSocket(ipport, sock);
        accept_callback_(ipport, (LPVOID)ipport.c_str());
        Receive(sock);
    }
    catch (const std::exception& ex)
    {
        SYS_ERROR("call accept callback error! %s", ex.what());
    }
}

void TcpServer::OnTimer(const boost::system::error_code &error) {
    if (!error && !is_canceled_) {
        socket_.cancel();
    }
}

void TcpServer::OnReceive(const boost::system::error_code &error, size_t bytes_transferred, sock_ptr_t sock) {
    try
    {
        std::string ipport = GetIPPort(sock);
        if (error) {
            if (!ipport.empty())
                SYS_ERROR("[%s] receive error : %s", ipport.c_str(), error.message().c_str());
            DelSocket(sock);
            return;
        }
        char str[kMaxDumpBufLen] = { 0 };
        SYS_TRACE("[%s] recv len[%d] data[%s]", ipport.c_str(), bytes_transferred, Tool::MemeryChar(recv_buf_, bytes_transferred, str, sizeof(str)));

        std::lock_guard<std::mutex> lock(mtx_recv_);
        if (bytes_transferred > 0)
        {
            boost::circular_buffer<uint8_t>* cache_buf = GetCacheBuf(ipport);
            if (!cache_buf)
            {
                SYS_ERROR("[%s] cache buf is null", ipport.c_str());
                return;
            }
            for (int i = 0; i < bytes_transferred; i++)
            {
                cache_buf->push_back(recv_buf_[i]);
            }
            if (cache_buf->size() >= cache_buf->capacity())
            {
                SYS_WARN_THROTTLE(LOG_INTERVAL_3S, "[%s] cache buf overflow! capacity[%d]", ipport.c_str(), cache_buf->capacity());
            }
            // notify HandleData
            cv_.notify_one();
        }
        // continue receive
        Receive(sock);
    }
    catch (std::exception& e)
    {
        SYS_ERROR("call recv callback error! %s", e.what());
        DelSocket(sock);
    }
}

void TcpServer::SetRecvCallback(recv_callback_t cb){
    recv_callback_ = cb;
}

void TcpServer::SetAcceptCallback(accept_callback_t cb)
{
    accept_callback_ = cb;
}

// 获取客户端数量
int TcpServer::GetClientsNum(){
    return map_clients_.size();
}

//处理数据
void TcpServer::HandleData()
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
        for (map_cache_t::iterator iter = map_cache_buf_.begin(); iter!= map_cache_buf_.end(); iter++)
        {
            total_len = 0;
            left_len = 0;
            std::string ipport = iter->first;
            cache_buf_t* cache_buf = iter->second;
            if (!cache_buf)
            {
                SYS_WARN("Get cache buf is null! ipport[%s]", ipport.c_str());
                continue;
            }
            total_len = cache_buf->size();
            // 待处理数据长度不足
            if (total_len <= 0 || total_len <= left_len)
            {
                boost::this_thread::sleep_for(boost::chrono::microseconds(100));
                continue;
            }
            left_len = total_len = total_len > sizeof(buf) ? sizeof(buf) : total_len;
            memset(buf, 0x00, sizeof(buf));
            //memcpy(buf, &cache_buf->at(0), total_len);
            for (int i = 0; i < total_len; i++)
            {
                buf[i] = cache_buf->at(i);
            }
            try
            {
                if (!recv_callback_.empty())
                {
                    recv_callback_(ipport, buf, left_len, null);
                }
                else
                {
                    left_len = 0; // 清空缓存
                    SYS_WARN("[%s] receive untreated msg: %s", ipport.c_str(), buf);
                }
            }
            catch (std::exception& e)
            {
                char str[kMaxDumpBufLen] = { 0 };
                SYS_ERROR("[%s] recv callback error[%s]. len[%d] data[%s]", ipport.c_str(), e.what(), left_len, Tool::MemeryChar(buf, left_len, str, sizeof(str)));
                left_len = 0; // 清空缓存
            }
            if (left_len < 0)
            {
                SYS_WARN("[%s] Parse frame error! erase cache!!! left[%d]", ipport.c_str(), left_len);
                left_len = 0;
            }
            cache_buf->erase_begin(total_len - left_len);
        }
    }
}

void TcpServer::Receive(sock_ptr_t sock) {
    SYS_DEBUG("start receive.(%s)", GetIPPort(sock).c_str());

    sock->async_read_some(boost::asio::buffer(recv_buf_),
            boost::bind(&TcpServer::OnReceive, GetSharedPtr(), boost::asio::placeholders::error,
                boost::asio::placeholders::bytes_transferred, sock));
}

void TcpServer::Receive()
{
    if (map_clients_.size() < 1) {
        SYS_WARN_THROTTLE(LOG_INTERVAL_3S, "No client online!");
        return;
    }
    for (auto iter = map_clients_.begin(); iter != map_clients_.end(); iter++)
    {
        Receive(iter->second);
    }
}

void TcpServer::Send(std::string sockname, std::string data) {
    sock_ptr_t sock_ptr = GeSocket(sockname);
    if (sock_ptr == NULL) {
        SYS_ERROR("Invalid socket! ipport[%s]", sockname.c_str());
        return;
    }
    char str[kMaxDumpBufLen] = { 0 };
    SYS_TRACE("[%s] send str[%s]", sockname.c_str(), data.c_str());
    boost::asio::async_write(*sock_ptr, boost::asio::buffer(data),
        strand_.wrap(boost::bind(&TcpServer::OnSend, GetSharedPtr(), boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred, sock_ptr)));
    return;
}

void TcpServer::Send(std::string data)
{
    if (map_clients_.size() < 1) {
        SYS_WARN_THROTTLE(LOG_INTERVAL_3S, "No client online!");
        return;
    }
    for (auto iter = map_clients_.begin(); iter != map_clients_.end(); iter++)
    {
        SYS_DEBUG("start send data to client.(%s)", (iter->first).c_str());
        Send(iter->first, data);
    }
    return;
}

void TcpServer::Send(std::string sockname, uint8_t* buf, uint32_t len)
{
    if (sockname.empty()) {
        SYS_WARN("Invalid socket! ipport[%s]", sockname.c_str());
        return;
    }
    sock_ptr_t sock_ptr = GeSocket(sockname);
    if (sock_ptr == NULL) {
        SYS_ERROR("Socket not found! ipport[%s]", sockname.c_str());
        return;
    }
    char str[kMaxDumpBufLen] = { 0 };
    SYS_TRACE("[%s] send len[%d] data[%s]", sockname.c_str(), len, Tool::MemeryChar(buf, len, str, sizeof(str)));

    boost::asio::async_write(*sock_ptr, boost::asio::buffer(buf, len),
        strand_.wrap(boost::bind(&TcpServer::OnSend, GetSharedPtr(), boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred, sock_ptr)));
    return;
}

void TcpServer::Send(uint8_t *buf, uint32_t len)
{
    if (map_clients_.size() < 1) {
        SYS_WARN_THROTTLE(LOG_INTERVAL_3S, "No client online!");
        return;
    }
    for (auto iter=map_clients_.begin(); iter!=map_clients_.end(); iter++)
    {
        SYS_DEBUG("start send data to client.(%s)", (iter->first).c_str());
        Send(iter->first, buf, len);
    }
    return;
}

// callback function for sending data
void TcpServer::OnSend(const boost::system::error_code &error, size_t bytes_transferred, sock_ptr_t sock) {
    if (error) {
        DelSocket(sock);
        SYS_ERROR("send error : %s", error.message().c_str());
    } else {
        SYS_DEBUG("send ok. %d", bytes_transferred);
    }
}

std::string TcpServer::GetIPPortFromEndpoint(sock_ptr_t sock)
{
    std::string ipport("");
    if (!sock)
        return ipport;
    try
    {
        ipport = sock->remote_endpoint().address().to_string() + ":" + std::to_string(sock->remote_endpoint().port());
    }
    catch (std::exception& ex)
    {
        SYS_ERROR("Call GetIPPort() exception! %s", ex.what());
        DelSocket(sock);
    }
    return ipport;
}

std::string TcpServer::GetIPPort(sock_ptr_t sock)
{
    for (auto iter = map_clients_.begin(); iter != map_clients_.end(); iter++)
    {
        if (iter->second == sock)
            return iter->first;
    }
    return std::string("");
}

void TcpServer::ShowAllClients()
{
    for (auto iter = map_clients_.begin(); iter != map_clients_.end(); iter++)
    {
        SYS_INFO("%s", iter->first.c_str());
    }
}

void TcpServer::AddSocket(std::string ipport, sock_ptr_t sock_ptr)
{
    std::unique_lock<std::mutex> lock(mtx_recv_);
    map_clients_[ipport] = sock_ptr;
    map_cache_buf_[ipport] = new boost::circular_buffer<uint8_t>(kMaxRecvBufLen);
    SYS_INFO("total %d clients online.", map_clients_.size());
    ShowAllClients();
}

void TcpServer::DelSocket(std::string ipport)
{
    std::unique_lock<std::mutex> lock(mtx_recv_);
    auto it = map_clients_.find(ipport);
    if (it != map_clients_.end())
    {
        sock_ptr_t sock = it->second;
        if (sock)
            sock->close();
        map_clients_.erase(it);
        SYS_INFO("remove socket from map. ipport[%s]", ipport.c_str());
    }
    auto it2 = map_cache_buf_.find(ipport);
    if (it2 != map_cache_buf_.end())
    {
        if (it2->second)
        {
            delete (it2->second);
        }
        map_cache_buf_.erase(it2);
        SYS_INFO("remove cache buf from map. ipport[%s]", ipport.c_str());
    }
    SYS_INFO("total %d clients online.", map_clients_.size());
    ShowAllClients();
}

void TcpServer::DelSocket(sock_ptr_t sock_ptr)
{
    std::lock_guard<std::mutex> lock(mtx_recv_);
    for (auto iter = map_clients_.begin(); iter != map_clients_.end(); )
    {
        if (iter->second == sock_ptr)
        {
            SYS_INFO("remove socket from map. ipport[%s]", iter->first.c_str());
            std::string ipport = iter->first;
            auto it2 = map_cache_buf_.find(ipport);
            if (it2 != map_cache_buf_.end())
            {
                if (it2->second)
                {
                    delete (it2->second);
                    it2->second = 0;
                }
                map_cache_buf_.erase(it2);
                SYS_INFO("remove cache buf from map. ipport[%s]", ipport.c_str());
            }
            map_clients_.erase(iter++);
        }
        else
        {
            iter++;
        }
    }
    if (sock_ptr)
        sock_ptr->close();
    SYS_INFO("total %d clients online.", map_clients_.size());
    ShowAllClients();
}

void TcpServer::ClearSocket()
{
    map_clients_.clear();
}

void TcpServer::Close()
{
    ClearSocket();
    socket_.close();
}

void TcpServer::Destroy()
{
    Close();
    io_service_.stop();
    threads_.interrupt_all();
    threads_.join_all();
}

void TcpServer::SetChannelName(std::string channel_name)
{
    channel_name_.assign(channel_name, 0, kMaxChannelNamePrefixLen);
    io_thread_name_ = channel_name_ + kChannelNameSuffixSrv;
    handle_thread_name_ = channel_name_ + kChannelNameSuffixHandle;
}

sock_ptr_t TcpServer::GeSocket(std::string ipport)
{
    auto it = map_clients_.find(ipport);

    if (it != map_clients_.end())
        return it->second;
    return NULL;
}

boost::circular_buffer<uint8_t>* TcpServer::GetCacheBuf(std::string ipport)
{
    return map_cache_buf_[ipport];
}

