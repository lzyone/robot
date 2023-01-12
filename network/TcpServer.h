#ifndef __ROCKBOT_TCPSERVER_H__
#define __ROCKBOT_TCPSERVER_H__

// headers in this package
#include "robot_config.h"
#include "CommChannel.h"

// headers in STL
#include <iostream>
#include <string>
#include <map>

// headers in boost
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/function.hpp>
#include <boost/bind.hpp>
#include <boost/thread.hpp>
#include <boost/chrono.hpp>

namespace rockbot 
{
namespace network
{
    // socket 指针
    typedef boost::shared_ptr<boost::asio::ip::tcp::socket> sock_ptr_t; 
    // map cache 类型
    typedef std::map<std::string, boost::circular_buffer<uint8_t>* > map_cache_t;
    // cache 类型
    typedef boost::circular_buffer<uint8_t> cache_buf_t;

    class finder
    {
    public:
        finder(const sock_ptr_t& cmp_sock_ptr) :s_(cmp_sock_ptr) {}
        bool operator ()(const std::map<std::string, sock_ptr_t>::value_type& item)
        {
            return item.second == s_;
        }
    private:
        const sock_ptr_t& s_;
    };

class TcpServer : public ServerChannel
{
public:
    TcpServer(int port);
    TcpServer(int port, std::string channel_name);
    TcpServer(int port, recv_callback_t cb);
    virtual ~TcpServer();

    // 开始
    virtual void Start() override;
    // 发送字符串
    virtual void Send(std::string sockname, std::string data) override;
    // 发送字符串
    virtual void Send(std::string data) override;
    // 发送字符数组
    virtual void Send(std::string sockname, uint8_t *buf, uint32_t len) override;
    // 发送给所有的客户端
    void Send(uint8_t *buf, uint32_t len) override;
    // 开始接收
    void Receive(sock_ptr_t sock);
    // 开始接收
    void Receive() override;
    // 设置接收回调函数
    void SetRecvCallback(recv_callback_t cb);
    // 设置新连接回调函数
    void SetAcceptCallback(accept_callback_t cb);
    // 获取客户端数量
    int GetClientsNum();
    // 处理数据
    virtual void HandleData() override;
    std::shared_ptr<TcpServer> GetSharedPtr() {
        return std::dynamic_pointer_cast<TcpServer>(shared_from_this());
    }
    // 关闭
    virtual void Close() override;
    // 销毁
    virtual void Destroy() override;

private:
    // 接收器
    boost::asio::ip::tcp::acceptor acceptor_; 
    // 客户端Map
    std::map<std::string, sock_ptr_t> map_clients_;
    // 接收回调
    recv_callback_t recv_callback_;
    // 新连接回调
    accept_callback_t accept_callback_;
    // 监听端口
    int port_;
    // 定时器
    boost::asio::steady_timer timer_;
    // 串行执行
    boost::asio::io_service::strand strand_;
    // 超时时间
    int timeout_;
    // 是否取消
    bool is_canceled_;
    // 服务端socket
    boost::asio::ip::tcp::socket socket_;
    // 接收buff
    uint8_t recv_buf_[kMaxRecvBufLen];
    // 缓存map
    map_cache_t map_cache_buf_;

    // 开始接受连接
    void StartAccept();
    // 接受连接回调
    void OnAccept(const boost::system::error_code& error, sock_ptr_t sock);
    // 发送回调
    void OnSend(const boost::system::error_code &error, size_t bytes_transferred, sock_ptr_t sock);
    // 接收回调
    void OnReceive(const boost::system::error_code &error, size_t bytes_transferred, sock_ptr_t sock);
    // 定时器回调
    void OnTimer(const boost::system::error_code &error);
    // 通过socket获取ip:port
    std::string GetIPPortFromEndpoint(sock_ptr_t sock_ptr);
    std::string GetIPPort(sock_ptr_t sock_ptr);
    // 通过ip:port获取socket
    sock_ptr_t GeSocket(std::string ipport);
    // 通过ip:port获取CacheBuf
    boost::circular_buffer<uint8_t>* GetCacheBuf(std::string ipport);
    // 添加socket
    void AddSocket(std::string ipport, sock_ptr_t sock_ptr);
    // 通过ip:port删除socket
    void DelSocket(std::string ipport);
    // 删除socket
    void DelSocket(sock_ptr_t sock_ptr);
    void ShowAllClients();
    // 清空所有socket
    void ClearSocket();
    // 设置通道名
    virtual void SetChannelName(std::string channel_name) override;
};
} // namespace network
} // namesapce rockbot
#endif  // __ROCKBOT_TCPSERVER_H__

