#include "process_worker.hpp"
#include <thread>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
#include "include.hpp"
#include "rtp_media_stream_processing.hpp"
#include "sip_header_processing.hpp"



using boost::interprocess::interprocess_mutex;
using boost::interprocess::scoped_lock;
using boost::interprocess::try_to_lock;


extern bool CONFIG_MODE_DUMP_RTP_STREAMS;
extern bool CONFIG_MODE_DUMP_SIP_HEADERS;


namespace worker {


uint32_t current_packet_cnt = 0;
uint16_t current_segment_cnt = 0;
struct shared_memory_segment* current_segment;
static std::deque<struct shared_memory_segment*> segments;
scoped_lock<interprocess_mutex> segment_lock;


void acquire_segment()
{
    uint16_t attempt = 0;

    while (true) {
        if (current_segment_cnt >= segments.size()) {
            current_segment_cnt = 0;
        }


        auto segment = segments.at(current_segment_cnt);

        scoped_lock<interprocess_mutex> lck(segment->mtx, try_to_lock);
        if (lck) {
            // looking only for captured
            if (!segment->network_packets_flushed && \
                segment->network_packets_captured && \
                !segment->network_packets_processed && \
                !segment->network_packets_assembled) {
                //std::cout << __func__ << " worker shm" << current_segment_cnt << std::endl;
    
                current_segment = segment;
                segment_lock = std::move(lck);

                break;
            } else {
                lck.unlock();
            }
        }

        ++current_segment_cnt;

        if (++attempt >= segments.size()) {
            attempt = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    current_packet_cnt = 0;
}

//----------------------------------------------------------------------

void release_segment()
{
    current_segment->network_packets_captured = false;
    current_segment->network_packets_processed = true;

    segment_lock.unlock();

    current_segment = nullptr;
    ++current_segment_cnt;
}

//----------------------------------------------------------------------

void process_segment()
{

// std::cout << "analyze segment  number#" << current_segment_cnt << std::endl;

    for (uint32_t i = 0; i < current_segment->data.size(); ++i) {
        auto& packet = current_segment->data.at(i);

        analyze(packet);
    }
}

//----------------------------------------------------------------------

void analyze(NetworkPacket& packet)
{
    uint8_t* ip_packet_ptr = packet.data() + ETHERNET_HEADER_LEN;
    uint16_t ip_packet_len = packet.size() - ETHERNET_HEADER_LEN;

    struct iphdr* ip_hdr = reinterpret_cast<struct iphdr*>(ip_packet_ptr);

    if (ip_hdr->version != 4) {
        packet.set_proto_unknown();
        return;
    }

    if (ip_hdr->protocol != IPPROTO_UDP) {
        packet.set_proto_unknown();
        return;
    }


    static constexpr uint16_t ip_hdr_len = sizeof(struct iphdr);
    static constexpr uint16_t udp_hdr_len = sizeof(struct udphdr);


    uint16_t payload_offset = ip_hdr_len + udp_hdr_len;
    if ((ip_packet_len - payload_offset) <= 0) {
        packet.set_proto_unknown();
        return;
    }

    struct udphdr* udp = (reinterpret_cast<struct udphdr*>(ip_packet_ptr + (ip_hdr->ihl << 2)));

    packet.set_src(ntohl(ip_hdr->saddr));
    packet.set_dst(ntohl(ip_hdr->daddr));
    packet.set_sport(ntohs(udp->source));
    packet.set_dport(ntohs(udp->dest));


    uint8_t* payload_ptr = ip_packet_ptr + payload_offset;
    uint16_t payload_len = ip_packet_len - payload_offset;


    if (packet.sport() == 5060 || packet.dport() == 5060) {
        analyze_sip_header(packet, payload_ptr, payload_len);
    } else {
        if (CONFIG_MODE_DUMP_RTP_STREAMS) {
            analyze_rtp_header(packet, payload_ptr, payload_len);
        }
    }
}

//----------------------------------------------------------------------

void analyze_sip_header(NetworkPacket& packet, const uint8_t* payload_ptr, uint16_t payload_len)
{

 std::cout << "analyze sip header"  << std::endl;

    std::string payload_str(reinterpret_cast<const char*>(payload_ptr), payload_len);
    get_sip_headers(packet.sip_meta(), payload_str);


    if (std::strlen(packet.sip_meta()->call_id())) {
        packet.set_proto_sip();
        
        std::cout << "call id        " << packet.sip_meta()->call_id() << std::endl;
        std::cout << "request method " << static_cast<uint16_t>(packet.sip_meta()->request_method()) << std::endl;
        std::cout << "response code  " << packet.sip_meta()->response_code() << std::endl;
        std::cout << "address        " << packet.sip_meta()->address() << std::endl;
        std::cout << "audio_port     " << packet.sip_meta()->audio_port() << std::endl;
        std::cout << "from           " << packet.sip_meta()->from() << std::endl;
        std::cout << "to             " << packet.sip_meta()->to() << std::endl;
        std::cout << std::endl;

        std::cout << payload_str << std::endl;

        std::cout << std::endl;
        
        
        return;
    } else {
        std::cout << "call id        " << packet.sip_meta()->call_id() << std::endl;
        std::cout << "request method " << static_cast<uint16_t>(packet.sip_meta()->request_method()) << std::endl;
        std::cout << "response code  " << packet.sip_meta()->response_code() << std::endl;
        std::cout << "address        " << packet.sip_meta()->address() << std::endl;
        std::cout << "audio_port     " << packet.sip_meta()->audio_port() << std::endl;
        std::cout << "from           " << packet.sip_meta()->from() << std::endl;
        std::cout << "to             " << packet.sip_meta()->to() << std::endl;
        std::cout << std::endl;

        std::cout << payload_str << std::endl;

        std::cout << std::endl;
    }

    packet.set_proto_unknown();
}

//----------------------------------------------------------------------

void analyze_rtp_header(NetworkPacket& packet, const uint8_t* payload_ptr, uint16_t payload_len)
{

//std::cout << "analyze RTP header"  << std::endl;

    auto hdr = reinterpret_cast<const struct rtp_header*>(payload_ptr);

    if (is_protocol_RTP(hdr, payload_len) && is_rtp_payload_type_allowed(hdr)) {
        auto hdr_len = get_rtp_header_len(hdr);

        const uint8_t* media_payload_ptr = payload_ptr + hdr_len;
        uint16_t media_payload_len = payload_len - hdr_len;


        auto meta = packet.rtp_meta();
        printf("analyze RP header len=%d\n", media_payload_len);
        meta->set_id(ntohl(hdr->sync_identifier));
        meta->set_sport(packet.sport());
        meta->set_dport(packet.dport());
        meta->set_sequence_number(ntohs(hdr->sequence_number));
        meta->set_codec(static_cast<Codec>(get_rtp_payload_type(hdr)));
        meta->set_payload_ptr(media_payload_ptr);
        meta->set_payload_len(media_payload_len);

        packet.set_proto_rtp();
        return;
    }

    packet.set_proto_unknown();
}


} // namespace worker


void process_worker()
{
    using namespace worker;

    std::cout << __func__ << " " << getpid() << std::endl;

    // wait for all processes to be started
    std::this_thread::sleep_for(std::chrono::seconds(1));

    segments = get_shared_memory_segments();
    if (segments.empty()) {
        exit_nicely();
    }

    while (true) {
        acquire_segment();
        process_segment();
        release_segment();
    }
}

