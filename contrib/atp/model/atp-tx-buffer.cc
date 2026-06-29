#include "atp-tx-buffer.h"
#include "atp-tag.h"
#include "ns3/log.h"
#include <cmath>

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
                    MakeTraceSourceAccessor(&ATPTxBuffer::m_lcwTrace),
                    "ns3::TracedValueCallback::Uint32");
  return tid;
}

ATPTxBuffer::ATPTxBuffer()
  : m_bufferSize(0), // 需要socket进行设置，比较重要
    m_bufferDataSize(0),
    m_packetNum(0),
    // PA-ATP初始化
    m_acw(1),
    m_lcw(1),
    // Progress Aware variables
    m_lastGACK(0),
    m_lastAACK(0),
    m_sAvg(0),

    m_alpha(0.0),
    m_beta(0.0),
    m_gamma(1.0),
    m_H(0.15),              // 论文推荐阈值
    m_virtualAcw(1.0),
    m_virtualLcw(1.0),
    m_packetsSentInPeriod(0),
    m_packetsToAggInPeriod(0),
    m_ecnCountInPeriod(0),
    m_aecnCountInPeriod(0),
    m_gackCountInPeriod(0),
    m_aackCountInPeriod(0)
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
        m_pendingQueue.pop();
        delete item;
    }
    
    // 清理已发送队列
    while (!m_sentQueue.empty())
    {
        ATPTxItem* item = m_sentQueue.front();
        m_sentQueue.pop();
        delete item;
    }

    // 清理重传队列
    while (!m_retxQueue.empty())
    {
        ATPTxItem* item = m_retxQueue.front();
        m_retxQueue.pop();
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
}

// 设置LCW（链路窗口）
void
ATPTxBuffer::SetLCW(uint32_t lcw)
{
    m_lcw = lcw;
    m_virtualLcw = static_cast<double>(lcw);
    m_lcwTrace = m_lcw;
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
    m_pendingQueue.push(item);
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
ATPTxBuffer::GetLastAACK() const
{
    return m_lastAACK;
}

void
ATPTxBuffer::UpdateProgressAACK(uint32_t ackNum)
{
    NS_LOG_FUNCTION(this << ackNum);
    m_lastAACK = ackNum;
}

// Helpers to update progress state from Socket (New Method)
uint32_t
ATPTxBuffer::GetLastGACK() const
{
    return m_lastGACK;
}

void
ATPTxBuffer::UpdateProgressGACK(uint32_t lastGACK, uint32_t sAvg)
{
    m_GACKs.insert(lastGACK);

    while (m_GACKs.count(m_lastGACK + 1) > 0)
    {
        m_lastGACK++;
        m_GACKs.erase(m_lastGACK);
    }
    if (sAvg > m_sAvg)
    {
        m_sAvg = sAvg;
    }
    m_virtualLcw += 1.0;
    if (m_virtualLcw > m_maxCwnd)
    {
        m_virtualLcw = m_maxCwnd;
    }
    m_lcw = static_cast<uint32_t>(m_virtualLcw);
}

Ptr<Packet>
ATPTxBuffer::SendPacket()
{
    NS_LOG_FUNCTION(this);

    // 检查是否有可用窗口
    if (m_pendingQueue.empty()) {
        NS_LOG_WARN("Buffer is empty, no packet to send");
        return nullptr;
    }

    // 发送新的数据包
    ATPTxItem* item = m_pendingQueue.front();
    m_pendingQueue.pop();
    m_bufferDataSize -= item->m_packet->GetSize();

    // 更新发送时间（微秒），将item存入已发送队列
    item->m_lastSentTime = static_cast<uint32_t>(Simulator::Now().GetMicroSeconds());
    m_sentQueue.push(item);
    // 记录发送统计
    m_packetsSentInPeriod++;
    m_highestSentPacketId = std::max(m_highestSentPacketId, item->m_packetId);

    return item->m_packet;
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
        m_sentQueue.pop();
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
            m_sentQueue.pop();
            
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
                m_retxQueue.push(retxItem);
            } else {
                // ID大于收到的ACK ID，保留
                tempQueue.push(front);
            }
        }

        // 将所有保留的包按原顺序放回m_sentQueue
        while (!tempQueue.empty()) {
            m_sentQueue.push(tempQueue.front());
            tempQueue.pop();
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

    if (type & 1)
    {
        if (type & 2)
        {
            m_ecnCountInPeriod++;
        }
        m_gackCountInPeriod++;
    }
    else
    {
        if (type & 2)
        {
            m_aecnCountInPeriod++;
        }
        m_aackCountInPeriod++;
    }
}

/**
 * PA-ATP Congestion Control Logic (Paper Section III-B)
 */
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
    double h_i = 0.0; // AECN标记比例

    if (m_gackCountInPeriod > 0)
    {
        e_i = static_cast<double>(m_ecnCountInPeriod) / m_gackCountInPeriod;
    }
    if (m_aackCountInPeriod > 0)
    {
        h_i = static_cast<double>(m_aecnCountInPeriod) / m_aackCountInPeriod;
    }

    // Update alpha (Link congestion level) - Eq 3.
    double f = 0.125; // Weight factor f from paper (Section VI-A)
    m_alpha = (1 - f) * m_alpha + f * e_i;

    // Update beta (Aggregator congestion level) - Eq 4.
    double w = 0.125; // Weight factor w from paper
    m_beta = (1 - w) * m_beta + w * h_i;

    // 2. Calculate Progress Degree p (Section III-B-2)
    // p_l = last_GACK - last_AACK
    // p_avg = s_avg - last_AACK
    // p = p_l / p_avg
    double p_avg = 0.0;
    if (m_sAvg >= m_lastAACK)
    {
        p_avg = static_cast<double>(m_sAvg - m_lastAACK);
    }

    double p_l = 0.0;
    if (m_lastGACK >= m_lastAACK)
    {
        p_l = static_cast<double>(m_lastGACK - m_lastAACK);
    }

    double p = 0.0;
    if (p_avg > 0)
    { // Avoid division by zero
        p = p_l / p_avg;
    }

    // p_max = AWD / p_avg
    double p_max = 1.0;
    if (p_avg > 0)
    {
        p_max = static_cast<double>(m_acw) / p_avg;
    }
    if (p > p_max)
        p = p_max; // Clamp p

    // 3. Update CWD (m_lcw) - Progress Aware
    // Factor d_i = alpha ^ (1 - p/p_max)
    if (e_i > 0) // If congestion exists
    {
        double exponent = 1.0;
        if (p_max > 0)
        {
            exponent = 1.0 - (p / p_max);
        }

        double d_i = std::pow(m_alpha, exponent);

        // Eq 1: CWD = CWD * (1 - d_i / 2)
        m_virtualLcw = m_virtualLcw * (1.0 - d_i / 2.0);
        m_lcw = static_cast<uint32_t>(std::max(1.0, m_virtualLcw));

        NS_LOG_INFO("PA-ATP CWD Reduce: alpha=" << m_alpha << " p=" << p << " p_max=" << p_max
                                                << " d_i=" << d_i << " new CWD=" << m_lcw);
    }

    // 4. Update AWD (m_acw) - Asynchronous Degree Control
    // Eq 2: AWD = AWD * (1 - beta / 2)
    if (h_i > 0)
    {
        m_virtualAcw = m_virtualAcw * (1.0 - m_beta / 2.0);
        m_acw = static_cast<uint32_t>(std::max(1.0, m_virtualAcw));
        NS_LOG_INFO("PA-ATP AWD Reduce: beta=" << m_beta << " new AWD=" << m_acw);
    }
    else
    {
        // Linear increase
        m_virtualAcw += 1.0;
        m_acw = static_cast<uint32_t>(m_virtualAcw);
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
    m_lcwTrace = m_lcw;

    NS_LOG_ERROR("e_i=" << e_i << " h_i=" << h_i);
    NS_LOG_INFO("PA-ATP windows adjusted: AWD=" << m_acw << " CWD=" << m_lcw);

    m_packetsSentInPeriod = 0;
    m_packetsToAggInPeriod = 0;
    m_ecnCountInPeriod = 0;
    m_aecnCountInPeriod = 0;
    m_gackCountInPeriod = 0;
    m_aackCountInPeriod = 0;
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
    m_retxQueue.pop();
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
        m_sentQueue.pop();
        
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
            retxItem->m_lastSentTime = currentTime;
            m_retxQueue.push(retxItem);

            item->m_lastSentTime = currentTime;
            tempQueue.push(item);
            timeoutCount++;
        } else {
            // 没有超时，保留在队列中
            tempQueue.push(item);
        }
    }
    
    // 将未超时的包放回已发送队列
    while (!tempQueue.empty()) {
        m_sentQueue.push(tempQueue.front());
        tempQueue.pop();
    }
    
    if (timeoutCount > 0) {
        NS_LOG_WARN("Found " << timeoutCount << " timeout packets");
    }
    
    return timeoutCount;
}

} // namespace ns3
