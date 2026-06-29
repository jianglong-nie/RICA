#ifndef ATP_TX_BUFFER_H
#define ATP_TX_BUFFER_H

#include "ns3/object.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/traced-value.h"

#include <queue>
#include <vector>
#include <set>

namespace ns3 {

/**
 * \brief ATP协议发送缓冲区的数据项
 */
class ATPTxItem
{
public:
  ATPTxItem() 
    : m_packet(nullptr),
      m_packetId(0),
      m_lastSentTime(0)
  {}
  ~ATPTxItem()
  {
    m_packet = nullptr;
  }

    Ptr<Packet> m_packet;       //!< 数据包
    uint32_t m_packetId;        //!< 数据包ID
    uint32_t m_lastSentTime;    //!< 最后发送时间（微秒）
};

/**
 * \brief ATP协议发送缓冲区实现
 */
class ATPTxBuffer : public Object
{
  public:
    static TypeId GetTypeId();
    
    ATPTxBuffer();
    ~ATPTxBuffer() override;

    /**
     * \brief 设置最大缓冲区大小
     * \param size 最大大小
     */
    void SetMaxBufferSize(uint32_t size);

    /**
     * \brief 设置拥塞窗口大小
     * \param cwnd 拥塞窗口大小
     */
    void SetACW(uint32_t cwnd);

    /**
     * \brief 设置拥塞窗口大小
     * \param cwnd 拥塞窗口大小
     */
    void SetLCW(uint32_t cwnd);

    /**
     * \brief 获取拥塞窗口大小
     * \return 拥塞窗口大小
     */
    uint32_t GetACW() const;

    /**
     * \brief 获取拥塞窗口大小
     * \return 拥塞窗口大小
     */
    uint32_t GetLCW() const;

    /**
     * \brief 添加数据到发送缓冲区
     * \param p 要添加的数据包
     * \return 是否添加成功
     */
    bool AddPacket(Ptr<Packet> p);

    /**
     * \brief 获取下一个要发送的数据包
     * \return 要发送的数据包，如果没有可发送的包则返回nullptr
     */
    Ptr<Packet> SendPacket();

    /**
     * \brief 检查ACK是否按序到达
     * \param packetId 收到的ACK包ID
     * \return 是否按序
     */
    bool IsOrderedAck(uint32_t packetId) const;

    /**
     * \brief 处理ACK
     * \param packetId 收到的ACK包ID
     * \return 是否按序
     */
    void ProcessOrderedAck(uint32_t packetId); 

    /**
     * \brief 处理乱序ACK
     * \param packetId 收到的ACK包ID
     * \return 是否按序
     */
    Ptr<Packet> ProcessUnorderedAck(uint32_t packetId);

    /**
     * \brief 处理拥塞状态
     * \param packetId 收到的ACK包ID
     */
    void ProcessCongestion(uint32_t packetId);

    /**
     * \brief 计数拥塞状态
     * \param type 拥塞类型（0 No, 1 isEcn, 2 collision）
     */
    void CountCongestion(int type);

    /**
     * \brief 检查是否需要重传
     * \return 是否需要重传
     */
    bool HasPacketToRetransmit() const;

    /**
     * \brief 重传数据包
     */
    Ptr<Packet> RetransmitPacket();
    
    /**
     * \brief 检查超时的数据包并移到重传队列
     * \param timeoutUs 超时时间（微秒）
     * \return 超时的数据包数量
     */
    uint32_t CheckAndMoveTimeoutPackets(uint32_t timeoutUs);

    /**
     * \brief 获取已发送队列的第一个数据包ID
     * \return 数据包ID
     */
    uint32_t GetSentPacketId() const;

    /**
     * \brief 获取待发送队列的第一个数据包ID
     * \return 数据包ID
     */
    uint32_t GetPendingFrontPacketId() const;

    /**
     * \brief 获取拥塞窗口左边界
     * \return 拥塞窗口左边界
     */
    uint32_t GetLastAACK() const;

    /**
     * \brief 更新拥塞窗口左边界
     * \param ackId 收到的AACK包ID
     */
    void UpdateProgressAACK(uint32_t ackNum);

    /**
     * \brief 获取m_lastGACK
     */
    uint32_t GetLastGACK() const;

    /**
     * \brief 更新进度信息
     * \param lastGACK 收到的上一个GACK包ID
     * \param sAvg sAvg
     */
    void UpdateProgressGACK(uint32_t lastGACK, uint32_t sAvg);

    typedef std::queue<ATPTxItem*> PacketQueue; //!< 数据包队列类型
    
    PacketQueue m_pendingQueue;         //!< 待发送数据队列
    PacketQueue m_sentQueue;            //!< 已发送数据队列
    PacketQueue m_retxQueue;            //!< 重传数据队列
    uint32_t m_bufferSize;              //!< 最大缓冲区大小
    uint32_t m_bufferDataSize;          //!< 缓冲区内的数据大小
    uint32_t m_packetNum;               //!< 进入缓冲区的数据包总数量

    // PA-ATP双窗口
    uint32_t m_acw;                     // 聚合器拥塞窗口
    uint32_t m_lcw;                     // 链路拥塞窗口
    TracedValue<uint32_t> m_lcwTrace;   // 拥塞窗口长度
    uint32_t m_lastGACK;                // 收到的上一个GACK包ID
    std::set<uint32_t> m_GACKs;         // 存储乱序到达的 GACK
    uint32_t m_lastAACK;                // 收到的上一个AACK包ID
    uint32_t m_sAvg;

    // PA-ATP统计量
    double m_alpha; // 聚合器拥塞等级（哈希碰撞率EMA）
    double m_beta;  // 链路拥塞等级（ECN标记率EMA）
    double m_gamma; // 掉队程度
    double m_H;     // 聚合器拥塞阈值（论文推荐0.15）

    // 拥塞控制参数
    uint32_t m_maxCwnd;  // 最大窗口大小
    double m_virtualAcw; // 虚拟ACW（浮点数）
    double m_virtualLcw; // 虚拟LCW（浮点数）

    // 当前调整周期内的统计
    uint32_t m_packetsSentInPeriod;    // 当前周期内发送的包数
    uint32_t m_packetsToAggInPeriod;   // 当前周期内发送到聚合器的包数
    uint32_t m_ecnCountInPeriod;       // 当前周期内的ECN标记数
    uint32_t m_aecnCountInPeriod;      // 当前周期内的碰撞数
    uint32_t m_gackCountInPeriod;      // 当前周期内收到的GACK数
    uint32_t m_aackCountInPeriod;      // 当前周期内收到的AACK数

    uint32_t m_roundEndPacketId{0}; // 记录本轮 RTT 期望等到的最大 Packet ID
    uint32_t m_highestSentPacketId{0};

    EventId m_windowIncreaseEvent;  //!< 窗口增长定时器事件
};

} // namespace ns3

#endif /* ATP_TX_BUFFER_H */
