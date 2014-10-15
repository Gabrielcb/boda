// Copyright (c) 2013-2014, Matthew W. Moskewicz <moskewcz@alumni.princeton.edu>; part of Boda framework; see LICENSE
#include"boda_tu_base.H"
#include"asio_util.H"
#include"geom_prim.H"
#include"timers.H"
#include"str_util.H"
#include"has_main.H"
#include"lexp.H"
#include"img_io.H"
#include"results_io.H"
#include"disp_util.H"
#include"cap_util.H"
#include"caffeif.H"
#include"pyif.H" // for py_boda_dir()

#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int_distribution.hpp>

namespace boda 
{
  struct cs_disp_t : virtual public nesi, public has_main_t // NESI(help="client-server video display test",
			  // bases=["has_main_t"], type_id="cs_disp")
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support
    uint32_t fps; //NESI(default=1000,help="camera poll rate: frames to try to capture per second (note: independent of display rate)")
    uint32_t debug; //NESI(default="0",help="set debug level (9 for max)")

    p_asio_alss_t disp_alss; 
    uint8_t in_redisplay;
    p_asio_alss_t proc_alss; 
    p_capture_t capture; //NESI(default="()",help="capture from camera options")    
    p_asio_fd_t cap_afd;

    uint8_t proc_done;
    p_img_t proc_out_img;

    uint32_t redisp_cnt;

    void on_redisplay_done( error_code const & ec ) { 
      assert_st( !ec );
      if( in_redisplay == 2 ) { 
	// stop capture
	capture->cap_stop(); 
	cap_afd->cancel(); 
	// stop proc
	uint8_t const quit = 2;
	bwrite( *proc_alss, quit );
	proc_alss->cancel();
	return; 
      }
      assert_st( in_redisplay == 0 );
      async_read( *disp_alss, buffer( (char *)&in_redisplay, 1), bind( &cs_disp_t::on_redisplay_done, this, _1 ) );
    }

    void do_redisplay( void ) {
      ++redisp_cnt;
      if( debug ) { 
	printf( "redisp_cnt=%s in_redisplay=%s\n", str(redisp_cnt).c_str(), str(uint32_t(in_redisplay)).c_str() ); }
      if( !in_redisplay ) {
	in_redisplay = 1;
	bwrite( *disp_alss, in_redisplay );
      } 
    }

    void on_proc_done( error_code const & ec ) { 
      if( ec == errc::operation_canceled ) { return; }
      assert_st( !ec );
      assert( proc_done );
#if 0 
      // for now, this is disabled/not possible/not useful, since: (1)
      // proc's out img is shared with the display (2) we update the
      // display frequently enough via the capture.  however, if
      // either changes we'd want to tweak this a bit. i.e. force
      // redisplay at (at leat) some FPS or the like, or change the
      // display code to update it's buffers every frame ...
      do_redisplay();
      if( !proc_done ) { // not done yet, just was progress update
	async_read( *proc_alss, buffer( (char *)&proc_done, 1), bind( &cs_disp_t::on_proc_done, this, _1 ) );
      }
#endif
    }

    void on_cap_read( error_code const & ec ) { 
      if( ec == errc::operation_canceled ) { return; }
      assert_st( !ec );
      bool const got_frame = capture->on_readable( 1 );
      //if( want_frame ) { printf( "want_frame=1 --> got_frame=%s\n", str(got_frame).c_str() ); }
      setup_capture_on_read( *cap_afd, &cs_disp_t::on_cap_read, this );
      if( got_frame ) { 
	do_redisplay();
	if( proc_done ) {
	  proc_done = 0;
	  bwrite( *proc_alss, proc_done );
	  bread( *proc_alss, proc_done ); // wait for input data to be saved/copied from cap_img in proc
	  assert_st( !proc_done );
	  async_read( *proc_alss, buffer( (char *)&proc_done, 1), bind( &cs_disp_t::on_proc_done, this,_1 ) );
	}
      }
    }

    cs_disp_t( void ) : in_redisplay(0), proc_done(1), redisp_cnt(0) { }

    p_io_service_t io;

    virtual void main( nesi_init_arg_t * nia ) { 
      io.reset( new io_service_t );
      create_boda_worker( *io, disp_alss, {"boda","display_ipc"} );
      create_boda_worker( *io, proc_alss, {"boda","proc_ipc"} );

      async_read( *disp_alss, buffer( (char *)&in_redisplay, 1), bind( &cs_disp_t::on_redisplay_done, this, _1 ) );

      multi_alss_t all_workers_alsss; all_workers_alsss.push_back(disp_alss); all_workers_alsss.push_back(proc_alss);

      capture->cap_img = make_and_share_p_img_t( all_workers_alsss, capture->cap_res );
      proc_out_img = make_and_share_p_img_t( all_workers_alsss, capture->cap_res );
      capture->cap_start();
      cap_afd.reset( new asio_fd_t( *io, ::dup(capture->get_fd() ) ) ); 
      setup_capture_on_read( *cap_afd, &cs_disp_t::on_cap_read, this );
      io->run();
    }
  };

  struct proc_ipc_t : virtual public nesi, public has_main_t // NESI(help="processing over ipc test",
			  // bases=["has_main_t"], type_id="proc_ipc")
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support
    int32_t boda_parent_socket_fd; //NESI(help="an open fd created by socketpair() in the parent process.",req=1)

    p_asio_alss_t alss;

    p_img_t in_img;
    p_img_t out_img;

    uint8_t proc_done;
    
    proc_ipc_t( void ) : proc_done(1) { }

    void on_parent_data( error_code const & ec ) { 
      assert_st( !ec );
      if( proc_done == 2 ) { return; } // quit command
      assert_st( proc_done == 0 );
      img_copy_to( in_img.get(), out_img.get(), 0, 0 );
      bwrite( *alss, proc_done ); // report input image captured/saved
      boost::random::uniform_int_distribution<> ry(0, out_img->h - 1);
      boost::random::uniform_int_distribution<> rx(0, out_img->w - 2);
      uint32_t rows_done = 0;
#pragma omp parallel
      {
	boost::random::mt19937 gen;
	while( 1 ) {
	  uint32_t * const rpd = out_img->get_row_pels_data( ry(gen) );
	  uint64_t num_swap = 0;
	  for( uint32_t j = 0; j < 1<<12; ++j ) {
	    uint32_t x1 = rx(gen);
	    uint32_t x2 = x1 + 1;
	    uint8_t y1,y2;
	    rgba2y( rpd[x1], y1 );
	    rgba2y( rpd[x2], y2 );
	    if( y1 < y2 ) { std::swap( rpd[x1], rpd[x2] ); ++num_swap; }
	  }
	  if( !num_swap ) { ++rows_done; }
	  if( rows_done > (out_img->h*300) ) { break; }
	}
      }

      // explicit progress update -- not needed with shared mem,
      // display always uses current out_img note: if we're in some
      // non-shared-mem mode/future, we probably want a time/fps
      // based update here, not per-some-unit-work. but that's
      // tricky if we're in some long, blocking compute process. i
      // suppose we could have a timer thread that copied the out
      // buf over the network explicitly every so often or the like
      // ... 
      // bwrite( *alss, proc_done );

      proc_done = 1;
      bwrite( *alss, proc_done );
      alss->async_read_some( buffer( &proc_done, 1 ), bind( &proc_ipc_t::on_parent_data, this, _1 ) );
    }

    virtual void main( nesi_init_arg_t * nia ) { 
      global_timer_log_set_disable_finalize( 1 );
      io_service_t io;
      alss.reset( new asio_alss_t(io)  );
      alss->assign( stream_protocol(), boda_parent_socket_fd );
      in_img = recv_shared_p_img_t( *alss );
      out_img = recv_shared_p_img_t( *alss );
      alss->async_read_some( buffer( &proc_done, 1 ), bind( &proc_ipc_t::on_parent_data, this, _1 ) );
      io.run();
    }
  };

  struct display_ipc_t : virtual public nesi, public has_main_t // NESI(help="video display over ipc test",
			  // bases=["has_main_t"], type_id="display_ipc")
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support
    int32_t boda_parent_socket_fd; //NESI(help="an open fd created by socketpair() in the parent process.",req=1)

    uint8_t parent_cmd;
    disp_win_t disp_win;
    p_asio_alss_t alss;

    void on_parent_data( error_code const & ec ) { 
      if( ec == errc::operation_canceled ) { return; }
      assert_st( !ec );
      //printf( "parent_cmd=%s\n", str(uint32_t(parent_cmd)).c_str() );
      disp_win.update_disp_imgs();
      uint8_t const in_redisplay = 0;
      bwrite( *alss, in_redisplay );
      async_read( *alss, buffer(&parent_cmd, 1), bind( &display_ipc_t::on_parent_data, this, _1 ) );
    }
    void on_quit( error_code const & ec ) { 
      alss->cancel(); 
      uint8_t const quit = 2;
      bwrite( *alss, quit );      
    }

    virtual void main( nesi_init_arg_t * nia ) { 
      global_timer_log_set_disable_finalize( 1 );
      io_service_t & io( get_io( &disp_win ) );
      alss.reset( new asio_alss_t(io)  );
      alss->assign( stream_protocol(), boda_parent_socket_fd );
      p_img_t img = recv_shared_p_img_t( *alss );
      p_img_t img2 = recv_shared_p_img_t( *alss );
      disp_win.disp_setup( vect_p_img_t{img,img2} );
      async_read( *alss, buffer(&parent_cmd, 1), bind( &display_ipc_t::on_parent_data, this, _1 ) );
      register_quit_handler( disp_win, &display_ipc_t::on_quit, this );
      io.run();
    }
  };

  // DEMOABLE items:
  //  -- sunglasses (wearing or on surface)
  //  -- keyboard (zoom in)
  //  -- loafer (better have both in view. socks optional)
  //  -- fruit/pear (might work ...)
  //  -- water bottle
  //  -- window shade
  //  -- teddy bear (more or less ...)
  //  -- coffee mug (great if centered)
  //  -- sandals 
  //  -- napkin (aka handkerchief, better be on flat surface) 
  //  -- paper towel (ehh, maybe ...)
  //  -- hat (aka mortarboard / cowboy hat )


  struct capture_classify_t : virtual public nesi, public has_main_t // NESI(help="cnet classifaction from video capture",
			      // bases=["has_main_t"], type_id="capture_classify")
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support
    // FIXME: needs to use timer / subject to epoll() bug ...
    p_capture_t capture; //NESI(default="()",help="capture from camera options")    
    p_cnet_predict_t cnet_predict; //NESI(default="()",help="cnet running options")    
    p_asio_fd_t cap_afd;
    disp_win_t disp_win;
    void on_cap_read( error_code const & ec ) { 
      assert_st( !ec );
      capture->on_readable( 1 );
      cnet_predict->do_predict( capture->cap_img ); 
      disp_win.update_disp_imgs();
      setup_capture_on_read( *cap_afd, &capture_classify_t::on_cap_read, this );
    }
    virtual void main( nesi_init_arg_t * nia ) { 
      cnet_predict->setup_predict(); 
      capture->cap_start();
      disp_win.disp_setup( capture->cap_img );

      io_service_t & io = get_io( &disp_win );
      cap_afd.reset( new asio_fd_t( io, ::dup(capture->get_fd() ) ) );
      setup_capture_on_read( *cap_afd, &capture_classify_t::on_cap_read, this );
      io.run();
    }
  };

  struct capture_feats_t : virtual public nesi, public has_main_t // NESI(help="cnet feature extraction from video capture",
			   // bases=["has_main_t"], type_id="capture_feats")
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support
    // FIXME: needs to use timer / subject to epoll() bug ...
    p_capture_t capture; //NESI(default="()",help="capture from camera options")    
    p_run_cnet_t run_cnet; //NESI(default="(ptt_fn=%(boda_test_dir)/conv_pyra_imagenet_deploy.prototxt,out_layer_name=conv3)",help="cnet running options")
    p_img_t feat_img;
    p_asio_fd_t cap_afd;
    disp_win_t disp_win;
    void on_cap_read( error_code const & ec ) { 
      assert_st( !ec );
      capture->on_readable( 1 );

      subtract_mean_and_copy_img_to_batch( run_cnet->in_batch, 0, capture->cap_img );
      p_nda_float_t out_batch = run_cnet->run_one_blob_in_one_blob_out();
      copy_batch_to_img( out_batch, 0, feat_img );
      disp_win.update_disp_imgs();
      setup_capture_on_read( *cap_afd, &capture_feats_t::on_cap_read, this );
    }
    virtual void main( nesi_init_arg_t * nia ) { 
      run_cnet->in_sz = capture->cap_res;
      run_cnet->setup_cnet(); 
      feat_img.reset( new img_t );
      u32_pt_t const feat_img_sz = run_cnet->get_one_blob_img_out_sz();
      feat_img->set_sz_and_alloc_pels( feat_img_sz.d[0], feat_img_sz.d[1] ); // w, h

      capture->cap_start();
      disp_win.disp_setup( vect_p_img_t{feat_img,capture->cap_img} );

      io_service_t & io = get_io( &disp_win );
      cap_afd.reset( new asio_fd_t( io, ::dup(capture->get_fd() ) ) );
      setup_capture_on_read( *cap_afd, &capture_feats_t::on_cap_read, this );
      io.run();
    }
  };


#include"gen/cap_app.cc.nesi_gen.cc"

}
