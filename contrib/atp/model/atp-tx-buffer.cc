#include "atp-tx-buffer.h"
#include "atp-tag.h"
#include "ns3/log.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("ATPTxBuffer");
NS_OBJECT_ENSURE_REGISTERED(ATPTxBuffer);

TypeId
ATPTxBuffer::GetTypeId()
{
  static TypeId tid = TypeId("ns3::ATPTxBuffer")
    .SetParent<Object>()
    .SetGroupName("Internet")
    .AddConstructor<ATPTxBuffer>()
    .AddTraceSource("cwndTrace",
                    "Aggregator congestion window length",
                    MakeTraceSourceAccessor(&ATPTxBuffer::m_acwTrace),
                    "ns3::TracedValueCallback::Uint32");
  return tid;
}

ATPTxBuffer::ATPTxBuffer()
  : m_bufferSize(0), // 需要socket进行设置，比较重要
    m_bufferDataSize(0),
    m_packetNum(0),
    // A²TP初始化
    m_acw(1),
    m_lcw(1),
    m_alpha(0.0),
    m_beta(0.0),
    m_gamma(1.0),
    m_H(0.15),              // 论文推荐阈值
    m_virtualAcw(1.0),
    m_virtualLcw(1.0),
    m_packetsSentInPeriod(0),
    m_packetsToAggInPeriod(0),
    m_ecnCountInPeriod(0),
    m_collisionCountInPeriod(0),
    m_ackCountInPeriod(0)
{
  NS_LOG_FUNCTION(this);
}

ATPTxBuffer::~ATPTxBuffer()
{
    NS_LOG_FUNCTION(this);
    
    // 取消定时器
    if (m_windowIncreaseEvent.IsPending()) {
        m_windowIncreaseEvent.Cancel();
    }
    
    // 清理待发送队列
    while (!m_pendingQueue.empty())
    {
        ATPTxItem* item = m_pendingQueue.front();
        m_pendingQueue.pop_front();
        delete item;
    }
    
    // 清理已发送队列
    while (!m_sentQueue.empty())
    {
        ATPTxItem* item = m_sentQueue.front();
        m_sentQueue.pop_front();
        delete item;
    }

    // 清理重传队列
    while (!m_retxQueue.empty())
    {
        ATPTxItem* item = m_retxQueue.front();
        m_retxQueue.pop_front();
        delete item;
    }
}

void
ATPTxBuffer::SetMaxBufferSize(uint32_t size)
{
    m_bufferSize = size;
    m_maxCwnd = size / 248;
}

// 设置ACW（聚合器窗口）
void
ATPTxBuffer::SetACW(uint32_t acw)
{
    m_acw = acw;
    m_virtualAcw = static_cast<double>(acw);

    // 确保ACW ≤ LCW
    if (m_acw > m_lcw)
    {
        m_acw = m_lcw;
        m_virtualAcw = m_lcw;
    }
    m_acwTrace = m_acw;
}

// 设置LCW（链路窗口）
void
ATPTxBuffer::SetLCW(uint32_t lcw)
{
    m_lcw = lcw;
    m_virtualLcw = static_cast<double>(lcw);

    // 确保ACW ≤ LCW
    if (m_acw > m_lcw)
    {
        m_acw = m_lcw;
        m_virtualAcw = m_virtualLcw;
    }
}

// 获取ACW和LCW
uint32_t
ATPTxBuffer::GetACW() const
{
    return m_acw;
}

uint32_t
ATPTxBuffer::GetLCW() const
{
    return m_lcw;
}

bool
ATPTxBuffer::AddPacket(Ptr<Packet> p)
{
    NS_LOG_FUNCTION(this << p);
    
    if (p->GetSize() + m_bufferDataSize > m_bufferSize)
    {
        NS_LOG_WARN("Buffer full, packet dropped");
        return false;
    }
    
    // 添加数据包到缓冲区
    auto item = new ATPTxItem();
    item->m_packet = p->Copy();
    m_packetNum++; // 数据包序号
    item->m_packetId = m_packetNum;
    m_pendingQueue.push_back(item);
    m_bufferDataSize += p->GetSize();
    
    return true;
}

uint32_t
ATPTxBuffer::GetSentPacketId() const
{
    if (m_sentQueue.empty()) {
        NS_LOG_WARN("GetSentPacketId: m_sentQueue is empty");
        return 0;
    }

    return m_sentQueue.front()->m_packetId;
}

uint32_t
ATPTxBuffer::GetPendingFrontPacketId() const
{
    if (m_pendingQueue.empty()) {
        NS_LOG_WARN("GetPendingFrontPacketId: m_pendingQueue is empty");
        return 0;
    }

    return m_pendingQueue.front()->m_packetId;
}

uint32_t
ATPTxBuffer::GetCwndLeftBound() const
{
    return m_leftBound;
}

void
ATPTxBuffer::UpdateCwndLeftBound(uint32_t ackNum)
{
    NS_LOG_FUNCTION(this << ackNum);
    m_leftBound = ackNum;
}
Ptr<Packet>
ATPTxBuffer::SendPacket(bool toAggregator)
{
    NS_LOG_FUNCTION(this);

    // 检查是否有可用窗口
    if (m_pendingQueue.empty()) {
        NS_LOG_WARN("Buffer is empty, no packet to send");
        return nullptr;
    }

    // 发送新的数据包
    ATPTxItem* item = m_pendingQueue.front();
    m_pendingQueue.pop_front();
    m_bufferDataSize -= item->m_packet->GetSize();

    // 更新发送时间（微秒），将item存入已发送队列
    item->m_lastSentTime = static_cast<uint32_t>(Simulator::Now().GetMicroSeconds());
    item->m_toAggregator = toAggregator;
    m_sentQueue.push_back(item);
    // 记录发送统计
    m_packetsSentInPeriod++;
    m_highestSentPacketId = std::max(m_highestSentPacketId, item->m_packetId);

    return item->m_packet;
}

uint32_t
ATPTxBuffer::GetAggregatorInFlight() const
{
    uint32_t count = 0;
    for (const auto item : m_sentQueue)
    {
        if (item->m_toAggregator)
        {
            count++;
        }
    }
    return count;
}

bool
ATPTxBuffer::IsOrderedAck(uint32_t packetId) const
{
    if (m_sentQueue.empty())
    {
        NS_LOG_WARN("IsOrderedAck: m_sentQueue is empty");
        return false;
    }

    ATPTxItem* item = m_sentQueue.front();
    uint32_t nextExpectedAckId = item->m_packetId;
    return packetId == nextExpectedAckId;
}

void
ATPTxBuffer::ProcessOrderedAck(uint32_t packetId)
{
    NS_LOG_FUNCTION(this << packetId);
    
    // 从m_sentQueue中移除已确认的包
    ATPTxItem* item = m_sentQueue.front();
    if (item->m_packetId == packetId)
    {
        m_sentQueue.pop_front();
        delete item;
    }
    else{  
        NS_LOG_WARN("Ordered ack, but packetId in m_sentQueue is not expected");
    }
}

Ptr<Packet>
ATPTxBuffer::ProcessUnorderedAck(uint32_t packetId)
{
    NS_LOG_FUNCTION(this << packetId);

    if (m_sentQueue.empty()) {
        NS_LOG_WARN("ProcessUnorderedAck: m_sentQueue is empty");
        return nullptr;
    }

    ATPTxItem* item = m_sentQueue.front();
    uint32_t nextExpectedAckId = item->m_packetId;

    if (packetId > nextExpectedAckId)
    {
        NS_LOG_WARN("Unordered ack received: " << packetId << ", expected: " << nextExpectedAckId);
        PacketQueue tempQueue;
        bool found = false;

        // 遍历队列找出所有需要重传的包
        while (!m_sentQueue.empty()) {
            ATPTxItem* front = m_sentQueue.front();
            m_sentQueue.pop_front();
            
            if (front->m_packetId == packetId) {
                // 找到匹配的包，删除它
                delete front;
                found = true;
            } else if (front->m_packetId < packetId) {
                // 创建新的重传项
                ATPTxItem* retxItem = new ATPTxItem();
                retxItem->m_packet = front->m_packet->Copy();
                retxItem->m_packetId = front->m_packetId;
                retxItem->m_lastSentTime = front->m_lastSentTime;
                m_retxQueue.push_back(retxItem);
            } else {
                // ID大于收到的ACK ID，保留
                tempQueue.push_back(front);
            }
        }

        // 将所有保留的包按原顺序放回m_sentQueue
        while (!tempQueue.empty()) {
            m_sentQueue.push_back(tempQueue.front());
            tempQueue.pop_front();
        }

        if (!found) {
            NS_LOG_WARN("ProcessUnorderedAck: Packet with ID " << packetId << " not found in sent queue");
        }
    }
    else{
        NS_LOG_WARN("Unordered ack, but packetId is less than nextExpectedAckId");
    }

    return item->m_packet;
}

void
ATPTxBuffer::CountCongestion(int type)
{
    NS_LOG_FUNCTION(this << type);

    if (type == 1 || type == 3)
    {
        m_ecnCountInPeriod++;
    }
    if (type == 2 || type == 3)
    {
        m_collisionCountInPeriod++;
    }
    m_ackCountInPeriod++;
}

void
ATPTxBuffer::ProcessCongestion(uint32_t packetId)
{
    if (m_roundEndPacketId == 0)
    {
        m_roundEndPacketId = m_highestSentPacketId;
    }
    if (packetId < m_roundEndPacketId)
    {
        return;
    }
    else
    {
        // 只要收到的 ACK 序列号达到了我们标记的终点，本轮宣告结束
        m_roundEndPacketId = m_highestSentPacketId;
    }
    // 计算统计量
    double e_i = 0.0; // ECN标记比例
    double h_i = 0.0; // 哈希碰撞比例

    if (m_ackCountInPeriod > 0)
    {
        e_i = static_cast<double>(m_ecnCountInPeriod) / m_ackCountInPeriod;
    }

    if (m_ackCountInPeriod > 0)
    {
        h_i = static_cast<double>(m_collisionCountInPeriod) / m_ackCountInPeriod;
    }

    // 计算掉队程度γ - 论文公式(3)
    double gamma_i = 1.0; // 默认无掉队
    if (m_lcw > 0)
    {
        gamma_i = static_cast<double>(m_ackCountInPeriod) / m_lcw;
        gamma_i = std::max(0.0, std::min(1.0, gamma_i)); // 限制在[0,1]
    }

    // 更新α和β的EMA - 论文公式(1)(2)
    double w = 1.0 / 16.0; // 论文权重
    double g = 1.0 / 16.0; // 论文权重

    m_alpha = (1 - w) * m_alpha + w * h_i;
    m_beta = (1 - g) * m_beta + g * e_i;
    m_gamma = gamma_i; // γ使用即时值，不是EMA

    NS_LOG_INFO("A²TP Adjustment - alpha=" << m_alpha << " beta=" << m_beta << " gamma=" << m_gamma
                                           << " ACW=" << m_acw << " LCW=" << m_lcw
                                           << " packets=" << m_packetsSentInPeriod
                                           << " acks=" << m_ackCountInPeriod);

    // 调整ACW（聚合器窗口）- 论文公式(4)(5)
    if (m_alpha > m_H)
    {
        // 计算聚合器拥塞程度p
        double p = (m_alpha - m_H) / (1 - m_H);

        // 计算减少因子d = p^γ
        double d = pow(p, m_gamma);

        // 调整ACW: ACW_{i+1} = ACW_i × (1 - d/2)
        m_virtualAcw = m_virtualAcw * (1 - d / 2.0);
        m_acw = static_cast<uint32_t>(std::max(1.0, m_virtualAcw));

        NS_LOG_INFO("ACW reduced: p=" << p << " d=" << d << " new ACW=" << m_acw);
    }
    else
    {
        // 没有聚合器拥塞，缓慢增加ACW（加性增加）
        m_virtualAcw += 1.0;
        m_acw = static_cast<uint32_t>(m_virtualAcw);
        NS_LOG_INFO("ACW increased to " << m_acw);
    }

    // 调整LCW（链路窗口）- 论文公式(6)
    if (e_i > 0)
    {
        // 调整LCW: LCW_{i+1} = LCW_i × (1 - β/2)
        m_virtualLcw = m_virtualLcw * (1 - m_beta / 2.0);
        m_lcw = static_cast<uint32_t>(std::max(1.0, m_virtualLcw));

        NS_LOG_INFO("LCW reduced: new LCW=" << m_lcw);
    }
    else
    {
        // 没有链路拥塞，缓慢增加LCW（加性增加）
        m_virtualLcw += 1.0;
        m_lcw = static_cast<uint32_t>(m_virtualLcw);
        NS_LOG_INFO("LCW increased to " << m_lcw);
    }

    // 确保ACW ≤ LCW
    if (m_acw > m_lcw)
    {
        m_acw = m_lcw;
        m_virtualAcw = m_virtualLcw;
        NS_LOG_INFO("ACW capped by LCW to " << m_acw);
    }

    // 窗口最大值限制
    if (m_acw > m_maxCwnd)
    {
        m_acw = m_maxCwnd;
        m_virtualAcw = m_maxCwnd;
    }

    if (m_lcw > m_maxCwnd)
    {
        m_lcw = m_maxCwnd;
        m_virtualLcw = m_maxCwnd;
    }
    m_acwTrace = m_acw;

    NS_LOG_INFO("A²TP windows adjusted: ACW=" << m_acw << " LCW=" << m_lcw);

    m_packetsSentInPeriod = 0;
    m_packetsToAggInPeriod = 0;
    m_ecnCountInPeriod = 0;
    m_collisionCountInPeriod = 0;
    m_ackCountInPeriod = 0;
}

bool
ATPTxBuffer::HasPacketToRetransmit() const
{
    return !m_retxQueue.empty();
}

Ptr<Packet>
ATPTxBuffer::RetransmitPacket()
{
    NS_LOG_FUNCTION(this);

    if (m_retxQueue.empty()) {
        NS_LOG_WARN("RetransmitPacket: m_retxQueue is empty");
        return nullptr;
    }

    ATPTxItem* item = m_retxQueue.front();
    m_retxQueue.pop_front();
    Ptr<Packet> packet = item->m_packet;
    delete item;

    return packet;
}

uint32_t
ATPTxBuffer::CheckAndMoveTimeoutPackets(uint32_t timeoutUs)
{
    NS_LOG_FUNCTION(this << timeoutUs);
    
    if (m_sentQueue.empty()) {
        return 0;
    }
    
    uint32_t currentTime = static_cast<uint32_t>(Simulator::Now().GetMicroSeconds());
    uint32_t timeoutCount = 0;
    PacketQueue tempQueue;
    
    // 遍历已发送队列，检查超时的包
    while (!m_sentQueue.empty()) {
        ATPTxItem* item = m_sentQueue.front();
        m_sentQueue.pop_front();
        
        uint32_t elapsedTime = currentTime - item->m_lastSentTime;
        
        if (elapsedTime >= timeoutUs) {
            // 超时了，移到重传队列
            NS_LOG_INFO("Packet " << item->m_packetId << " timeout, elapsed: " 
                        << elapsedTime << "us, threshold: " << timeoutUs << "us");

            ATPTxItem* retxItem = new ATPTxItem();
            retxItem->m_packet = item->m_packet->Copy();
            // 设置 ATPTag 的 resend = 1
            ATPTag atpTag;
            if (retxItem->m_packet->PeekPacketTag(atpTag)) {
                atpTag.m_resend = 1;
                retxItem->m_packet->ReplacePacketTag(atpTag);
            }
            retxItem->m_packetId = item->m_packetId;
            retxItem->m_lastSentTime = item->m_lastSentTime;
            m_retxQueue.push_back(retxItem);

            tempQueue.push_back(item);
            timeoutCount++;
        } else {
            // 没有超时，保留在队列中
            tempQueue.push_back(item);
        }
    }
    
    // 将未超时的包放回已发送队列
    while (!tempQueue.empty()) {
        m_sentQueue.push_back(tempQueue.front());
        tempQueue.pop_front();
    }
    
    if (timeoutCount > 0) {
        NS_LOG_WARN("Found " << timeoutCount << " timeout packets");
    }
    
    return timeoutCount;
}

} // namespace ns3
