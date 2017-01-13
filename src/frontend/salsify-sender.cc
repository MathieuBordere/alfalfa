/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include <cstdlib>
#include <iostream>
#include <chrono>
#include <vector>
#include <random>
#include <limits>
#include <thread>
#include <future>
#include <algorithm>

#include "yuv4mpeg.hh"
#include "encoder.hh"
#include "socket.hh"
#include "packet.hh"
#include "poller.hh"
#include "socketpair.hh"
#include "exception.hh"

using namespace std;
using namespace std::chrono;
using namespace PollerShortNames;

struct EncodeJob
{
  uint32_t frame_no;

  RasterHandle raster;

  Encoder encoder;
  EncoderMode mode;

  uint8_t y_ac_qi;
  size_t target_size;

  EncodeJob( const uint32_t frame_no, RasterHandle raster, const Encoder & encoder )
    : frame_no( frame_no ), raster( raster ), encoder( encoder ),
      mode( CONSTANT_QUANTIZER ), y_ac_qi(), target_size()
  {}
};

struct EncodeOutput
{
  Encoder encoder;
  vector<uint8_t> frame;
  chrono::milliseconds encode_time;

  EncodeOutput( Encoder && encoder, vector<uint8_t> && frame, const chrono::milliseconds encode_time )
    : encoder( move( encoder ) ), frame( move( frame ) ),
      encode_time( encode_time )
  {}
};

EncodeOutput do_encode_job( EncodeJob && encode_job )
{
  vector<uint8_t> output;

  const auto encode_beginning = chrono::system_clock::now();

  switch ( encode_job.mode ) {
  case CONSTANT_QUANTIZER:
    output = encode_job.encoder.encode_with_quantizer( encode_job.raster.get(),
                                                       encode_job.y_ac_qi );
    break;

  case TARGET_FRAME_SIZE:
    output = encode_job.encoder.encode_with_target_size( encode_job.raster.get(),
                                                         encode_job.target_size );
    break;

  default:
    throw runtime_error( "unsupported encoding mode." );
  }

  const auto encode_ending = chrono::system_clock::now();
  const auto ms_elapsed = chrono::duration_cast<chrono::milliseconds>( encode_ending - encode_beginning );

  return { move( encode_job.encoder ), move( output ), ms_elapsed };
}

size_t target_size( uint32_t avg_delay, const uint64_t last_acked, const uint64_t last_sent )
{
  uint32_t max_delay = 100 * 1000; // 100 ms = 100,000 us

  if ( avg_delay == 0 ) { avg_delay = 1; }

  cerr << "Packets in flight: " << last_sent - last_acked << "\n";
  cerr << "Avg inter-packet-arrival interval: " << avg_delay << "\n";
  cerr << "Imputed delay: " << avg_delay * (last_sent - last_acked) << " us\n";

  return 1400 * max( 0l, static_cast<int64_t>( max_delay / avg_delay - ( last_sent - last_acked ) ) );
}

void usage( const char *argv0 )
{
  cerr << "Usage: " << argv0 << " QUANTIZER HOST PORT CONNECTION_ID" << endl;
}

unsigned int paranoid_atoi( const string & in )
{
  const unsigned int ret = stoul( in );
  const string roundtrip = to_string( ret );
  if ( roundtrip != in ) {
    throw runtime_error( "invalid unsigned integer: " + in );
  }
  return ret;
}

int main( int argc, char *argv[] )
{
  /* check the command-line arguments */
  if ( argc < 1 ) { /* for sticklers */
    abort();
  }

  if ( argc != 5 ) {
    usage( argv[ 0 ] );
    return EXIT_FAILURE;
  }

  /* open the YUV4MPEG input */
  YUV4MPEGReader input { FileDescriptor( STDIN_FILENO ) };

  /* get quantizer argument */
  const unsigned int y_ac_qi = paranoid_atoi( argv[ 1 ] );

  /* get connection_id */
  const uint16_t connection_id = paranoid_atoi( argv[ 4 ] );

  /* construct Socket for outgoing datagrams */
  UDPSocket socket;
  socket.connect( Address( argv[ 2 ], argv[ 3 ] ) );
  socket.set_timestamps();

  /* average inter-packet delay, reported by receiver */
  uint32_t avg_delay = numeric_limits<uint32_t>::max();

  /* keep the number of fragments per frame */
  vector<uint64_t> cumulative_fpf;
  uint64_t last_acked = numeric_limits<uint64_t>::max();

  /* maximum number of frames to be skipped in a row */
  const size_t MAX_SKIPPED = 5;
  size_t skipped_count = 0;

  /* frame rate */
  static const int MS_PER_SECOND = 1000;
  uint8_t fps = 12;
  chrono::milliseconds time_per_frame { MS_PER_SECOND / fps };

  /* construct the encoder */
  Encoder encoder { input.display_width(), input.display_height(),
                    false /* two-pass */, REALTIME_QUALITY };

  /* encoded frame index */
  unsigned int frame_no = 0;

  /* latest raster that is received from the input */
  Optional<RasterHandle> last_raster;

  /* where we keep the outputs of parallel encoding jobs */
  vector<EncodeJob> encode_jobs;
  vector<future<EncodeOutput>> encode_outputs;

  auto encode_started_pipe = UnixDomainSocket::make_pair();
  auto encode_ended_pipe = UnixDomainSocket::make_pair();

  thread(
    [&]()
    {
      while ( true ) {
        encode_started_pipe.first.write( "1" );
        this_thread::sleep_for( time_per_frame );
      }
    }
  ).detach();

  Poller poller;

  /* fetch frames from webcam */
  poller.add_action( Poller::Action( input.fd(), Direction::In,
    [&]() -> Result {
      last_raster = input.get_next_frame();

      if ( not last_raster.initialized() ) {
        return { ResultType::Exit, EXIT_FAILURE };
      }

      return ResultType::Continue;
    },
    [&]() { return not input.fd().eof(); } )
  );

  /* start the encoding jobs for the next frame.
     this action is signaled by a thread every ( 1 / fps ) seconds. */
  poller.add_action( Poller::Action( encode_started_pipe.second, Direction::In,
    [&]()
    {
      encode_started_pipe.second.read();

      if ( encode_jobs.size() > 0 ) {
        /* a frame is being encoded now */
        return ResultType::Continue;
      }

      if ( not last_raster.initialized() ) {
        /* there is no raster, it's only yourself. */
        return ResultType::Continue;
      }

      RasterHandle raster = last_raster.get();

      const auto encode_deadline = chrono::system_clock::now() + time_per_frame;

      cerr << "Preparing encoding jobs for frame #" << frame_no << "." << endl;
      EncodeJob encode_job { frame_no, raster, encoder };

      if ( true or avg_delay == numeric_limits<uint32_t>::max() ) {
        encode_job.mode = CONSTANT_QUANTIZER;
        encode_job.y_ac_qi = y_ac_qi;
      }
      else {
        size_t frame_size = target_size( avg_delay, last_acked, cumulative_fpf.back() );

        if ( frame_size <= 0 and skipped_count < MAX_SKIPPED ) {
          skipped_count++;
          cerr << "skipping frame." << endl;
          return ResultType::Continue;
        }
        else if ( frame_size == 0 ) {
          cerr << "too many skipped frames, let's send one with a low quality." << endl;
          encode_job.mode = TARGET_FRAME_SIZE;
          encode_job.target_size = 1400; // One MTU
        }
        else {
          cerr << "encoding with target size=" << frame_size << endl;
          encode_job.mode = TARGET_FRAME_SIZE;
          encode_job.target_size = frame_size;
        }
      }

      encode_jobs.push_back( move( encode_job ) );

      // this thread will spawn all the encoding jobs and will wait on the results
      thread(
        [&]()
        {
          encode_outputs.clear();
          encode_outputs.reserve( encode_jobs.size() );

          for ( auto & job : encode_jobs ) {
            encode_outputs.push_back( async( launch::async, do_encode_job, move( job ) ) );
          }

          for ( auto & future_res : encode_outputs ) {
            future_res.wait_until( encode_deadline );
          }

          encode_ended_pipe.first.write( "1" );
        }
      ).detach();

      cerr << "Running " << encode_jobs.size() << " encoding job(s)." << endl;

      return ResultType::Continue;
    },
    [&]() { return not input.fd().eof(); } )
  );

  poller.add_action( Poller::Action( encode_ended_pipe.second, Direction::In,
    [&]()
    {
      encode_ended_pipe.second.read();

      auto validity_predicate = [&]( const future<EncodeOutput> & o ) { return o.valid(); };

      if ( not any_of( encode_outputs.cbegin(), encode_outputs.cend(), validity_predicate ) ) {
        // no encoding job has ended in time
        encode_jobs.clear();
        return ResultType::Continue;
      }

      auto out_future = find_if( encode_outputs.begin(), encode_outputs.end(), validity_predicate );
      auto output = move( ( *out_future ).get() );

      cerr << "Encoding time: " << output.encode_time.count() << " ms" << endl;

      cerr << "Sending frame #" << frame_no << "...";
      FragmentedFrame ff { connection_id,
                           frame_no,
                           static_cast<uint32_t>( duration_cast<microseconds>( time_per_frame ).count() ),
                           output.frame };
      ff.send( socket );
      cerr << "done." << endl;

      cumulative_fpf.push_back( ( frame_no > 0 ) ? ( cumulative_fpf[ frame_no - 1 ] + ff.fragments_in_this_frame() )
                                : ff.fragments_in_this_frame() );

      encoder = move( output.encoder );
      skipped_count = 0;
      frame_no++;

      encode_jobs.clear();

      return ResultType::Continue;
    },
    [&]() { return not input.fd().eof(); } )
  );

  poller.add_action( Poller::Action( socket, Direction::In,
    [&]()
    {
      auto packet = socket.recv();
      AckPacket ack( packet.payload );

      if ( ack.connection_id() != connection_id ) {
        /* this is not an ack for this session! */
        return ResultType::Continue;
      }

      avg_delay = ack.avg_delay();

      last_acked = ( ack.frame_no() > 0 )
                   ? ( cumulative_fpf[ ack.frame_no() - 1 ] + ack.fragment_no() )
                   : ack.fragment_no();

      return ResultType::Continue;
    },
    [&]() { return not input.fd().eof(); } )
  );

  /* handle events */
  while ( true ) {
    const auto poll_result = poller.poll( -1 );
    if ( poll_result.result == Poller::Result::Type::Exit ) {
      return poll_result.exit_status;
    }
  }

  return EXIT_FAILURE;
}
