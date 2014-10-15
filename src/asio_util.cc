#include"boda_tu_base.H"
#include"asio_util.H"

namespace boda 
{

#include"gen/build_info.cc"
  string get_boda_shm_filename( void ) { return strprintf( "/boda-rev-%s-pid-%s-top.shm", build_rev, 
							   str(getpid()).c_str() ); }

  void create_boda_worker( io_service_t & io, p_asio_alss_t & alss, vect_string const & args ) {
    int sp_fds[2];
    neg_one_fail( socketpair( AF_LOCAL, SOCK_STREAM, 0, sp_fds ), "socketpair" );
    set_fd_cloexec( sp_fds[0], 0 ); // we want the parent fd closed in our child
    vect_string fin_args = args;
    fin_args.push_back( strprintf("--boda-parent-socket-fd=%s",str(sp_fds[1]).c_str() ) );
    fork_and_exec_self( fin_args );
    neg_one_fail( close( sp_fds[1] ), "close" ); // in the parent, we close the socket child will use
    alss.reset( new asio_alss_t(io)  );
    alss->assign( stream_protocol(), sp_fds[0] );
  }


}
