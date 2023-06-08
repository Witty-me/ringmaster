#include <getopt.h>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <chrono>

#include "conversion.hh"
#include "udp_socket.hh"
#include "sdl.hh"
#include "protocol.hh"
#include "decoder.hh"

using namespace std;
using namespace chrono;

void print_usage(const string & program_name)
{
  cerr <<
  "Usage: " << program_name << " [options] port\n\n"
  "Options:\n"
  "--lazy <level>       0: decode and display frames (default)\n"
  "                     1: decode but not display frames\n"
  "                     2: neither decode nor display frames\n"
  "-o, --output <file>  file to output performance results to\n"
  "-v, --verbose        enable more logging for debugging"
  << endl;
}

pair<Address, ConfigMsg> recv_config_msg(UDPSocket & udp_sock)
{
  // wait until a valid ConfigMsg is received
  while (true) {
    const auto & [peer_addr, raw_data] = udp_sock.recvfrom();

    const shared_ptr<Msg> msg = Msg::parse_from_string(raw_data.value());
    if (msg == nullptr or msg->type != Msg::Type::CONFIG) {
      continue; // ignore invalid or non-config messages
    }

    const auto config_msg = dynamic_pointer_cast<ConfigMsg>(msg);
    if (config_msg) {
      return {peer_addr, *config_msg};
    }
  }
}

int main(int argc, char * argv[])
{
  // argument parsing
  int lazy_level = 0;
  string output_path;
  bool verbose = false;

  const option cmd_line_opts[] = {
    {"lazy",    required_argument, nullptr, 'L'},
    {"output",  required_argument, nullptr, 'o'},
    {"verbose", no_argument,       nullptr, 'v'},
    { nullptr,  0,                 nullptr,  0 },
  };

  while (true) {
    const int opt = getopt_long(argc, argv, "o:v", cmd_line_opts, nullptr);
    if (opt == -1) {
      break;
    }

    switch (opt) {
      case 'L':
        lazy_level = strict_stoi(optarg);
        break;
      case 'o':
        output_path = optarg;
        break;
      case 'v':
        verbose = true;
        break;
      default:
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
  }

  if (optind != argc - 1) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  const auto port = narrow_cast<uint16_t>(strict_stoi(argv[optind]));


  UDPSocket udp_sock;
  udp_sock.bind({"0", port});
  cerr << "Local address: " << udp_sock.local_address().str() << endl;

  // wait for a receiver to send 'ConfigMsg' and "connect" to it
  cerr << "Waiting for sender..." << endl;

  const auto & [peer_addr, config_msg] = recv_config_msg(udp_sock);
  cerr << "Peer address: " << peer_addr.str() << endl;
  udp_sock.connect(peer_addr);

  // read configuration from the peer
  const auto width = config_msg.width;
  const auto height = config_msg.height;
  const auto frame_rate = config_msg.frame_rate;
  const auto target_bitrate = config_msg.target_bitrate;

  cerr << "Received config: width=" << to_string(width)
       << " height=" << to_string(height)
       << " FPS=" << to_string(frame_rate)
       << " bitrate=" << to_string(target_bitrate) << endl;

  // initialize decoder
  Decoder decoder(width, height, lazy_level, output_path);
  decoder.set_verbose(verbose);

  // main loop
  while (true) {
    // parse a datagram received from sender
    Datagram datagram;
    if (not datagram.parse_from_string(udp_sock.recv().value())) {
      throw runtime_error("failed to parse a datagram");
    }

    // send an ACK back to sender
    AckMsg ack(datagram);
    udp_sock.send(ack.serialize_to_string());

    if (verbose) {
      cerr << "Acked datagram: frame_id=" << datagram.frame_id
           << " frag_id=" << datagram.frag_id << endl;
    }

    // process the received datagram in the decoder
    decoder.add_datagram(move(datagram));

    // check if the expected frame(s) is complete
    while (decoder.next_frame_complete()) {
      // depending on the lazy level, might decode and display the next frame
      decoder.consume_next_frame();
    }
  }

  return EXIT_SUCCESS;
}
