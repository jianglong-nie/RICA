#include "atp-tag.h"
#include "atp-l4-protocol.h"
#include "atp-socket.h"
#include "atp-socket-factory.h"
#include "atp-static-routing.h"
#include "atp-header.h"

#include "ns3/ipv4-end-point-demux.h"
#include "ns3/ipv4-end-point.h"
#include "ns3/ipv4-route.h"
#include "ns3/ipv4.h"
#include "ns3/ipv4-routing-protocol.h"

#include "ns3/assert.h"
#include "ns3/log.h"
#include "ns3/node.h"
#include "ns3/object-map.h"
#include "ns3/packet.h"

#include <unordered_map>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ATPL4Protocol");

NS_OBJECT_ENSURE_REGISTERED(ATPL4Protocol);

// TBD: Need to assign a protocol number for ATP
const uint8_t ATPL4Protocol::PROT_NUMBER = 142; 

TypeId
ATPL4Protocol::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::ATPL4Protocol")
            .SetParent<IpL4Protocol>()
            .SetGroupName("Internet")
            .AddConstructor<ATPL4Protocol>()
            .AddAttribute("SocketList",
                         "The list of sockets associated to this protocol.",
                         ObjectMapValue(),
                         MakeObjectMapAccessor(&ATPL4Protocol::m_sockets),
                         MakeObjectMapChecker<ATPSocket>())
            .AddTraceSource("Drop",
                           "Trace source indicating a packet has been dropped at L4 layer",
                           MakeTraceSourceAccessor(&ATPL4Protocol::m_dropTrace),
                           "ns3::Packet::TracedCallback");
    return tid;
}

ATPL4Protocol::ATPL4Protocol()
    : m_endPoints(new Ipv4EndPointDemux())
{
    NS_LOG_FUNCTION(this);

    m_aggregators.resize(MAX_AGGREGATORS);
}

ATPL4Protocol::~ATPL4Protocol()
{
    NS_LOG_FUNCTION(this);
}

void
ATPL4Protocol::SetNode(Ptr<Node> node)
{
    m_node = node;
}

// 
void
ATPL4Protocol::NotifyNewAggregate()
{
    NS_LOG_FUNCTION(this);
    Ptr<Node> node = this->GetObject<Node>();
    Ptr<Ipv4> ipv4 = this->GetObject<Ipv4>();

    if (!m_node)
    {
        if (node && ipv4)
        {
            this->SetNode(node);
            Ptr<ATPSocketFactory> atpFactory = CreateObject<ATPSocketFactory>();
            atpFactory->SetATP(this);
            node->AggregateObject(atpFactory);
        }
    }

    if (ipv4 && m_downTarget.IsNull())
    {
        ipv4->Insert(this);
        this->SetDownTarget(MakeCallback(&Ipv4::Send, ipv4));
    }
    
    IpL4Protocol::NotifyNewAggregate();
}

int
ATPL4Protocol::GetProtocolNumber() const
{
    return PROT_NUMBER;
}

void
ATPL4Protocol::DoDispose()
{
    NS_LOG_FUNCTION(this);
    for (auto i = m_sockets.begin(); i != m_sockets.end(); i++)
    {
        i->second = nullptr;
    }
    m_sockets.clear();

    if (m_endPoints != nullptr)
    {
        delete m_endPoints;
        m_endPoints = nullptr;
    }
    
    m_node = nullptr;
    m_downTarget.Nullify();
    IpL4Protocol::DoDispose();
}

Ptr<Socket>
ATPL4Protocol::CreateSocket()
{
    NS_LOG_FUNCTION(this);
    Ptr<ATPSocket> socket = CreateObject<ATPSocket>();
    socket->SetNode(m_node);
    socket->SetATP(this);
    m_sockets[m_socketIndex++] = socket;
    return socket;
}

bool
ATPL4Protocol::RemoveSocket(Ptr<ATPSocket> socket)
{
    NS_LOG_FUNCTION(this << socket);

    for (auto& socketItem : m_sockets)
    {
        if (socketItem.second == socket)
        {
            socketItem.second = nullptr;
            m_sockets.erase(socketItem.first);
            return true;
        }
    }
    return false;
}

Ipv4EndPoint*
ATPL4Protocol::Allocate()
{
    NS_LOG_FUNCTION(this);
    return m_endPoints->Allocate();
}

Ipv4EndPoint*
ATPL4Protocol::Allocate(Ipv4Address address)
{
    NS_LOG_FUNCTION(this << address);
    return m_endPoints->Allocate(address);
}

Ipv4EndPoint*
ATPL4Protocol::Allocate(Ptr<NetDevice> boundNetDevice, uint16_t port)
{
    NS_LOG_FUNCTION(this << boundNetDevice << port);
    return m_endPoints->Allocate(boundNetDevice, port);
}

Ipv4EndPoint*
ATPL4Protocol::Allocate(Ptr<NetDevice> boundNetDevice, Ipv4Address address, uint16_t port)
{
    NS_LOG_FUNCTION(this << boundNetDevice << address << port);
    return m_endPoints->Allocate(boundNetDevice, address, port);
}

Ipv4EndPoint*
ATPL4Protocol::Allocate(Ptr<NetDevice> boundNetDevice,
                        Ipv4Address localAddress,
                        uint16_t localPort,
                        Ipv4Address peerAddress,
                        uint16_t peerPort)
{
    NS_LOG_FUNCTION(this << boundNetDevice << localAddress << localPort << peerAddress << peerPort);
    return m_endPoints->Allocate(boundNetDevice, localAddress, localPort, peerAddress, peerPort);
}

void
ATPL4Protocol::DeAllocate(Ipv4EndPoint* endPoint)
{
    NS_LOG_FUNCTION(this << endPoint);
    m_endPoints->DeAllocate(endPoint);
}

void
ATPL4Protocol::OnAggAcquire(uint8_t jobId)
{
    if (!m_enableAggCount)
    {
        return;
    }
    auto it = m_aggCount.find(jobId);
    if (it != m_aggCount.end())
    {
        ++(it->second);
    }
    else
    {
        m_aggCount[jobId] = 1;
    }
}

void
ATPL4Protocol::OnAggRelease(uint8_t jobId)
{
    if (!m_enableAggCount)
    {
        return;
    }
    auto it = m_aggCount.find(jobId);
    if (it != m_aggCount.end() && it->second > 0)
    {
        --(it->second);
    }
}

Ptr<Packet>
ATPL4Protocol::FilterPacket(Ptr<Packet> packet)
{
    NS_LOG_FUNCTION(this << packet);

    Ptr<Packet> forwardedPacket = nullptr;

    ATPTag atpTag;
    bool hasATPTag = packet->PeekPacketTag(atpTag);

    if (!hasATPTag) {
        forwardedPacket = packet->Copy();
        return forwardedPacket;
    }

    if (atpTag.m_isAck == 1) {
        std::size_t index = Aggregator::HashToIndex(atpTag.GetJobId(), atpTag.GetSeqNum(), MAX_AGGREGATORS);
        NS_LOG_INFO("ATPL4Protocol: Reset aggregator for jobId " << static_cast<int>(atpTag.GetJobId())
        << " seqNum " << static_cast<int>(atpTag.GetSeqNum()));

        // 获取对应的聚合器
        Aggregator& aggregator = m_aggregators[index];
        if (aggregator.m_jobId == atpTag.GetJobId() && aggregator.m_seqNum == atpTag.GetSeqNum())
        {
            aggregator.Reset();
            OnAggRelease(atpTag.GetJobId());
        }
        forwardedPacket = packet->Copy();
    }
    else if (atpTag.m_collision == 1) {
        forwardedPacket = packet->Copy();
    }
    else if (!m_enableHierarchicalAggregation && atpTag.m_edgeSwitchIdentifier == 1)
    {
        forwardedPacket = packet->Copy();
    }
    else {
        forwardedPacket = AggregatePacket(packet->Copy());

        if (forwardedPacket == nullptr)
        {
            NS_LOG_INFO("Packet is waiting for aggregation.");
        }
    }

    return forwardedPacket;
}

Ptr<Packet>
ATPL4Protocol::AggregatePacket(Ptr<Packet> packet)
{
    NS_LOG_FUNCTION(this << packet);
    
    ATPTag atpTag;
    packet->PeekPacketTag(atpTag);

    // 使用Aggregator类的哈希函数计算索引
    std::size_t index = Aggregator::HashToIndex(atpTag.GetJobId(), atpTag.GetSeqNum(), MAX_AGGREGATORS);
    
    // 获取对应的聚合器
    Aggregator& aggregator = m_aggregators[index];

    // 处理重传包
    if (atpTag.m_resend == 1) {
        if (aggregator.m_jobId == atpTag.GetJobId() 
        && aggregator.m_seqNum == atpTag.GetSeqNum()) {
            if (atpTag.m_edgeSwitchIdentifier == 0) {
                uint32_t temp_bitmap = atpTag.m_bitmap0;
                // 判断当前包是否被聚合过了
                if ((temp_bitmap & aggregator.m_bitmap) == temp_bitmap) {
                    aggregator.Reset();
                    OnAggRelease(atpTag.GetJobId());
                    return packet;
                }
                else {
                    aggregator.m_bitmap = aggregator.m_bitmap | temp_bitmap;
                    atpTag.m_edgeSwitchIdentifier += 1;
                    atpTag.m_bitmap0 = aggregator.m_bitmap;
                    packet->ReplacePacketTag(atpTag);
                    aggregator.Reset();
                    OnAggRelease(atpTag.GetJobId());
                    return packet;
                }
            }
            else {
                aggregator.Reset();
                OnAggRelease(atpTag.GetJobId());
                return packet;
            }
        }
        else {
            return packet;
        }
    }

    // 如果聚合器为空，或者jobId和seqNum都匹配
    if (aggregator.IsEmpty() || 
        (aggregator.m_jobId == atpTag.GetJobId() && 
         aggregator.m_seqNum == atpTag.GetSeqNum()))
    {
        // 如果是空的，初始化jobId和seqNum
        if (aggregator.IsEmpty()) {
            aggregator.m_jobId = atpTag.GetJobId();
            aggregator.m_seqNum = atpTag.GetSeqNum();
            OnAggAcquire(atpTag.GetJobId());
        }

        // 添加数据包到聚合器
        bool aggregationComplete = aggregator.ProcessPacket(packet->Copy());
        if (aggregationComplete)
        {
            // 聚合完成，发送聚合后的数据包
            Ptr<Packet> aggPacket = aggregator.GetResultPacket();
            Ptr<Packet> aggregatedPacket = aggPacket->Copy();
            
            ATPTag atpTag;
            aggregatedPacket->PeekPacketTag(atpTag);

            NS_LOG_INFO("Aggregated packet: " << atpTag);


            return aggregatedPacket;
        }
        else
        {
            return nullptr;
        }
    }
    else{
        packet->RemovePacketTag(atpTag);

        atpTag.m_resend = 1;
        atpTag.m_collision = 1;

        packet->AddPacketTag(atpTag);


        m_jobIdHashCollisionCounter[atpTag.GetJobId()]++;

        NS_LOG_INFO("Hash Collision!");
        return packet;
    }
}

// 需要做相关修改
IpL4Protocol::RxStatus
ATPL4Protocol::Receive(Ptr<Packet> packet, const Ipv4Header& header, Ptr<Ipv4Interface> interface)
{
    NS_LOG_FUNCTION(this << packet << header);
    ATPHeader atpHeader;
    packet->RemoveHeader(atpHeader);

    ATPTag atpTag;

    // 只是peek ATP头部
    packet->PeekPacketTag(atpTag);

    // 查找匹配的端点
    NS_LOG_DEBUG("Looking up dst " << header.GetDestination() << " port "
                                  << atpTag.GetDestinationPort());
    Ipv4EndPointDemux::EndPoints endPoints = m_endPoints->Lookup(header.GetDestination(),
                                                                atpTag.GetDestinationPort(),
                                                                header.GetSource(),
                                                                atpTag.GetSourcePort(),
                                                                interface);
    if (endPoints.empty())
    {
        NS_LOG_LOGIC("RX_ENDPOINT_UNREACH");
        m_dropTrace(packet, "No matching endpoint");
        return IpL4Protocol::RX_ENDPOINT_UNREACH;
    }

    
    // 将数据包转发给所有匹配的端点
    NS_ASSERT_MSG(endPoints.size() == 1, "ATP expects exactly one endpoint");
    NS_LOG_LOGIC("ATPL4Protocol " << this
                                  << " received a packet and"
                                     " now forwarding it up to endpoint/socket");

    (*endPoints.begin())->ForwardUp(packet, header, atpTag.GetSourcePort(), interface);

    return IpL4Protocol::RX_OK;
}

IpL4Protocol::RxStatus
ATPL4Protocol::Receive(Ptr<Packet> packet,
                       const Ipv6Header& header,
                       Ptr<Ipv6Interface> interface)
{
    NS_LOG_FUNCTION(this << packet << header);
    return IpL4Protocol::RX_ENDPOINT_UNREACH;
}

void
ATPL4Protocol::Send(Ptr<Packet> packet,
                   Ipv4Address saddr,
                   Ipv4Address daddr,
                   uint16_t sport,
                   uint16_t dport)
{
    NS_LOG_FUNCTION(this << packet << saddr << daddr << sport << dport);

    // 1. 提取packet中的atptag
    ATPTag initTag;
    
    bool hasATPTag = packet->PeekPacketTag(initTag);
    packet->RemovePacketTag(initTag);

    if (hasATPTag)
    {
        // 设置Tag
        NS_LOG_INFO("Get ATPTag from packet");
        ATPTag atpTag;

        // 使用CopyFrom函数复制所有值
        atpTag.CopyFrom(initTag);
        
        // 更新端口信息
        atpTag.SetSourcePort(sport);
        atpTag.SetDestinationPort(dport);

        packet->AddPacketTag(atpTag);

        ATPHeader atpHeader;
        packet->AddHeader(atpHeader);

        // 发送数据包
        m_downTarget(packet, saddr, daddr, PROT_NUMBER, nullptr);
    }
    else
    {
        NS_LOG_INFO("No ATPTag in packet");
        m_downTarget(packet, saddr, daddr, PROT_NUMBER, nullptr);
    }
}

void
ATPL4Protocol::Send(Ptr<Packet> packet,
                   Ipv4Address saddr,
                   Ipv4Address daddr,
                   uint16_t sport,
                   uint16_t dport,
                   Ptr<Ipv4Route> route)
{
    NS_LOG_FUNCTION(this << packet << saddr << daddr << sport << dport << route);

    // 1. 提取packet中的atptag
    ATPTag initTag;

    bool hasATPTag = packet->PeekPacketTag(initTag);
    packet->RemovePacketTag(initTag);

    if (hasATPTag)
    {
        // 设置Tag
        NS_LOG_INFO("Get ATPTag from packet");
        ATPTag atpTag;
        
        // 使用CopyFrom函数复制所有值
        atpTag.CopyFrom(initTag);

        // 更新端口信息
        atpTag.SetDestinationPort(dport);
        atpTag.SetSourcePort(sport);
        
        packet->AddPacketTag(atpTag);

        ATPHeader atpHeader;
        packet->AddHeader(atpHeader);

        // 4. 发送数据包
        m_downTarget(packet, saddr, daddr, PROT_NUMBER, route);
    }
    else
    {
        NS_LOG_INFO("No ATPTag in packet");
        m_downTarget(packet, saddr, daddr, PROT_NUMBER, route);
    }
}

void
ATPL4Protocol::SetDownTarget(IpL4Protocol::DownTargetCallback callback)
{
    NS_LOG_FUNCTION(this);
    m_downTarget = callback;
}

void
ATPL4Protocol::SetDownTarget6(IpL4Protocol::DownTargetCallback6 callback)
{
    NS_LOG_FUNCTION(this);
    m_downTarget6 = callback;
}

IpL4Protocol::DownTargetCallback
ATPL4Protocol::GetDownTarget() const
{
    return m_downTarget;
}

IpL4Protocol::DownTargetCallback6
ATPL4Protocol::GetDownTarget6() const
{
    return m_downTarget6;
}

} // namespace ns3
