#ifndef ATP_L4_PROTOCOL_H
#define ATP_L4_PROTOCOL_H

#include "ns3/ip-l4-protocol.h"
#include "ns3/packet.h"
#include "ns3/ptr.h"
#include "ns3/traced-callback.h"
#include "ns3/simulator.h"
#include "atp-aggregator.h"

#include <stdint.h>
#include <map>
#include <vector>

namespace ns3
{

class ATPTag;
class Node;
class Socket;
class ATPSocket;
class Ipv4EndPoint;
class Ipv4EndPointDemux;
class Ipv6EndPoint;
class Ipv6EndPointDemux;
class NetDevice;

/**
 * \ingroup atp
 * \brief Implementation of the ATP protocol
 */
class ATPL4Protocol : public IpL4Protocol
{
  public:
    static TypeId GetTypeId();
    static const uint8_t PROT_NUMBER; //!< protocol number (0xFE)

    ATPL4Protocol();
    ~ATPL4Protocol() override;

    // Delete copy constructor and assignment operator to avoid misuse
    ATPL4Protocol(const ATPL4Protocol&) = delete;
    ATPL4Protocol& operator=(const ATPL4Protocol&) = delete;

    void SetNode(Ptr<Node> node);
    int GetProtocolNumber() const override;
    Ptr<Socket> CreateSocket();
    bool RemoveSocket(Ptr<ATPSocket> socket);

    void ResetOverflowCount(uint8_t jobId, uint32_t seqNum);

    Ipv4EndPoint* Allocate();
    Ipv4EndPoint* Allocate(Ipv4Address address);
    Ipv4EndPoint* Allocate(Ptr<NetDevice> boundNetDevice, uint16_t port);
    Ipv4EndPoint* Allocate(Ptr<NetDevice> boundNetDevice, Ipv4Address address, uint16_t port);
    Ipv4EndPoint* Allocate(Ptr<NetDevice> boundNetDevice,
                          Ipv4Address localAddress,
                          uint16_t localPort,
                          Ipv4Address peerAddress,
                          uint16_t peerPort);

    void DeAllocate(Ipv4EndPoint* endPoint);

    void Send(Ptr<Packet> packet,
             Ipv4Address saddr,
             Ipv4Address daddr,
             uint16_t sport,
             uint16_t dport);
    void Send(Ptr<Packet> packet,
             Ipv4Address saddr,
             Ipv4Address daddr,
             uint16_t sport,
             uint16_t dport,
             Ptr<Ipv4Route> route);

    // From IpL4Protocol
    IpL4Protocol::RxStatus Receive(Ptr<Packet> p,
                                 const Ipv4Header& header,
                                 Ptr<Ipv4Interface> interface) override;
    
    IpL4Protocol::RxStatus Receive(Ptr<Packet> p,
                                 const Ipv6Header& header,
                                 Ptr<Ipv6Interface> interface) override;

    void SetDownTarget(IpL4Protocol::DownTargetCallback cb) override;
    void SetDownTarget6(IpL4Protocol::DownTargetCallback6 cb) override;
    IpL4Protocol::DownTargetCallback GetDownTarget() const override;
    IpL4Protocol::DownTargetCallback6 GetDownTarget6() const override;
    
    void SetEnableAggregation(bool enable);
    Ptr<Packet> AggregatePacket(Ptr<Packet> packet);
    Ptr<Packet> FilterPacket(Ptr<Packet> packet, const Ipv4Header& ipHeader);

    uint32_t GetJobIdHashCollisionCounter(uint8_t jobId) const { 
      auto it = m_jobIdHashCollisionCounter.find(jobId);
      if (it != m_jobIdHashCollisionCounter.end()) {
        return it->second;
      }
      return 0;
    };

    uint32_t GetAggregatorCount(uint8_t jobId) const
    {
        if (!m_enableAggCount)
            return 0;
        auto it = m_aggCount.find(jobId);
        if (it != m_aggCount.end())
        {
            return it->second;
        }
        return 0;
    }

    void EnableAggregatorCount(bool enable)
    {
        m_enableAggCount = enable;
    }
  
  protected:
    void DoDispose() override;
    void NotifyNewAggregate() override;

  private:
    void OnAggAcquire(uint8_t jobId);
    void OnAggRelease(uint8_t jobId);

    Ptr<Node> m_node;                //!< The node this stack is associated with
    Ipv4EndPointDemux* m_endPoints;  //!< A list of IPv4 end points.

    std::unordered_map<uint64_t, Ptr<ATPSocket>>
        m_sockets;             //!< Unordered map of socket IDs and corresponding sockets
    uint64_t m_socketIndex{0}; //!< Index of the next socket to be created
    IpL4Protocol::DownTargetCallback m_downTarget;   //!< Callback to send packets over IPv4
    IpL4Protocol::DownTargetCallback6 m_downTarget6; //!< Callback to send packets over IPv6

    static const uint32_t MAX_AGGREGATORS = 2048;  //!< Maximum number of aggregators
    std::vector<Aggregator> m_aggregators;         //!< Vector of aggregators

    double m_eta{0.3};
    int32_t m_overflowCount{0};
    int32_t m_usedAggregators{0};
    static const uint32_t BDP = 416; //!< counted by packet

    //hash collision counter <jobId, collision count>
    std::unordered_map<uint8_t, uint32_t> m_jobIdHashCollisionCounter{{0, 0}, {1, 0}};

    //the switch maintaining cumulative sequence numbers (S_sum in the paper)
    std::map<uint8_t, uint32_t> m_jobSeqSum;

    // PA-ATP: Store S_n for each Worker in each Job (JobId -> (WorkerIP -> LastSeqNum))
    // Read_Write_Register_S_n Table in the paper
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> m_workerLastSeq;

    std::map<std::pair<uint32_t, uint32_t>, int32_t> m_pendingOverflows;

    // occupancy tracking
    std::unordered_map<uint8_t, uint32_t> m_aggCount{{0, 0}, {1, 0}};
    bool m_enableAggCount{false};

    /**
     * Trace source for packets dropped at L4 layer
     */
    TracedCallback<Ptr<const Packet>, const char *> m_dropTrace;
};

} // namespace ns3

#endif /* ATP_L4_PROTOCOL_H */
