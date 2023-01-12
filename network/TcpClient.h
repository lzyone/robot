#ifndef __ROCKBOT_TCPCLIENT_H__
#define __ROCKBOT_TCPCLIENT_H__

// headers in this package
#include "robot_config.h"
#include "CommChannel.h"

// headers in STL
#include <iostream>

// headers in boost
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/function.hpp>
#include <boost/bind.hpp>
#include <boost/thread.hpp>

namespace rockbot 
{
namespace network
{
class TcpClient : public ClientChannel
{
public:
    TcpClient(std::string ip_address, int port, int timeout = kDefaultTimeout);
    TcpClient(std::string ip_address, int port, std::string channel_name, int interval = 0, int timeout = kDefaultTimeout);

    virtual ~TcpClient();
    // 开始 
    virtual void Start() override;
    // 发送字符串
    void Send(std::string data);
    // 发送字符数组
    void Send(uint8_t *buf, uint32_t len);
    // 设置接收回调
    void SetRecvCallback(recv_callback_t cb);
    // 开始接收
    void Receive();
    // 处理数据
    void HandleData();
    // 重新打开
    virtual void ReOpen() override;
    // 关闭
    virtual void Close() override;
    // 销毁
    virtual void Destroy() override;
    // 设置通道名
    virtual void SetChannelName(std::string channel_name) override;
    // 获取连接状态
    inline bool connected() { return connected_; }
    std::shared_ptr<TcpClient> GetSharedPtr() {
        return std::dynamic_pointer_cast<TcpClient>(shared_from_this());
    }

private:
    // 客户端socket
    boost::asio::ip::tcp::socket socket_;
    // 服务端IP
    std::string ip_address_;
    // 服务端端口
    int port_;
    // 正在连接
    volatile bool is_connecting_;
    // 定时器
    boost::asio::steady_timer timer_;
    // 串行执行
    boost::asio::io_service::strand strand_;
    // 超时时间
    int timeout_;
    // 重试时间间隔
    int retry_interval_;
    // 是否取消
    bool is_canceled_;
    // 接收buff
    uint8_t recv_buf_[kMaxRecvBufLen];
    // 接收回调
    std::vector<recv_callback_t> vec_recv_callback_;
    // 连接服务端
    int Connect();
    // 连接回调
    void OnConnect(const boost::system::error_code &error);
    // 发送回调
    void OnSend(const boost::system::error_code &error, size_t bytes_transferred);
    void OnSend(const boost::system::error_code &error, std::vector<uint8_t>* data, size_t bytes_transferred);
    // 接收回调
    void OnReceive(const boost::system::error_code &error, size_t bytes_transferred);
    // 定时器回调
    void OnTimer(const boost::system::error_code &error);
    // 处理发送缓存
    void HandleSendCache();
};
} // namespace network
} // namesapce rockbot
#endif  // __ROCKBOT_TCPCLIENT_H__

