#ifndef __ROCKBOT_ASYNCSERIAL_H__
#define __ROCKBOT_ASYNCSERIAL_H__

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
class AsyncSerial : public ClientChannel
{
public:
    AsyncSerial(std::string dev_name, uint32_t baud_rate,
        boost::asio::serial_port_base::parity opt_parity = boost::asio::serial_port_base::parity(boost::asio::serial_port_base::parity::none),
        boost::asio::serial_port_base::character_size opt_csize = boost::asio::serial_port_base::character_size(8),
        boost::asio::serial_port_base::flow_control opt_flow = boost::asio::serial_port_base::flow_control( boost::asio::serial_port_base::flow_control::none),
        boost::asio::serial_port_base::stop_bits opt_stop = boost::asio::serial_port_base::stop_bits( boost::asio::serial_port_base::stop_bits::one));
    AsyncSerial(std::string dev_name, std::string channel_name, uint32_t baud_rate, uint32_t interval = 0,
        boost::asio::serial_port_base::parity opt_parity = boost::asio::serial_port_base::parity(boost::asio::serial_port_base::parity::none),
        boost::asio::serial_port_base::character_size opt_csize = boost::asio::serial_port_base::character_size(8),
        boost::asio::serial_port_base::flow_control opt_flow = boost::asio::serial_port_base::flow_control(boost::asio::serial_port_base::flow_control::none),
        boost::asio::serial_port_base::stop_bits opt_stop = boost::asio::serial_port_base::stop_bits(boost::asio::serial_port_base::stop_bits::one));
    virtual ~AsyncSerial();
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
    // 设置通道名
    virtual void SetChannelName(std::string channel_name) override;
    // 获取连接状态
    inline bool connected() { return connected_; }
    // 处理数据
    void HandleData();
    std::shared_ptr<AsyncSerial> GetSharedPtr() {
        return std::dynamic_pointer_cast<AsyncSerial>(shared_from_this());
    }
    // 重新打开
    virtual void ReOpen() override;
    // 关闭
    virtual void Close() override;
    // 销毁
    virtual void Destroy() override;

private:
    // 串口
    boost::asio::serial_port sp_;
    // 设备地址
    std::string dev_name_;
    // 端口号
    int com_no_;
    // 错误码
    boost::system::error_code ec_;
    //波特率
    boost::asio::serial_port::baud_rate baud_rate_;
    //奇偶校验，可以为serial_port::parity::none / odd / even。
    boost::asio::serial_port_base::parity opt_parity_;
    //字符大小
    boost::asio::serial_port_base::character_size opt_csize_;
    //流量控制， 可以为serial_port::flow_control::type，enum类型，可以是none software hardware
    boost::asio::serial_port_base::flow_control opt_flow_;
    //停止位，可以为serial_port::stop_bits::one /onepointfive /two
    boost::asio::serial_port_base::stop_bits opt_stop_;
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
    // 连接串口
    int Open();
    // 连接回调
    //void OnConnect(const boost::system::error_code &error);
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
#endif  // __ROCKBOT_ASYNCSERIAL_H__

