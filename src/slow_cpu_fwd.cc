// Copyright (c) 2015, Matthew W. Moskewicz <moskewcz@alumni.princeton.edu>; part of Boda framework; see LICENSE
#include"boda_tu_base.H"
#include"str_util.H"
#include"has_conv_fwd.H"
#include"timers.H"
#include"conv_util.H"

namespace boda 
{

  struct slow_cpu_fwd_t : virtual public nesi, public has_conv_fwd_t // NESI(help="compute conv pipe forward using slow, simple cpu code",
			   // bases=["has_conv_fwd_t"], type_id="slow_cpu" )

  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support
    uint32_t skip_work; //NESI(default=0,help="if non-zero, skip all work")

    p_conv_pipe_t cp;

    virtual void init( p_conv_pipe_t const & cp_ );
    virtual void run_fwd( vect_string const & to_set_vns, p_map_str_p_nda_float_t const & fwd, vect_string const & to_get_vns );
    virtual string get_info_log( void ) { return string(); }
    virtual void set_det_drop_seed( uint32_t const & det_drop_seed_ ) { assert_st(0); } // FIXME: TODO ... along with everything else in this file ;)

  };

  void slow_cpu_fwd_t::init( p_conv_pipe_t const & cp_ ) { cp = cp_; assert_st( cp );  }

  void run_conv_op_one_img_conv( p_conv_op_t const & cop, p_map_str_p_nda_float_t const & fwd, uint32_t const img_ix, 
				 p_nda_float_t const & bot, p_nda_float_t const & top ) {    
    u32_pt_t kern_sz = cop->kern_sz;
    if( kern_sz.is_zeros() ) { kern_sz = {bot->dims.dims(3), bot->dims.dims(2)}; } // 'global' input special case
    p_nda_float_t const & filts = must_find( *fwd, cop->tag + "_filts" );
    p_nda_float_t const & biases = must_find( *fwd, cop->tag + "_biases" );
    assert_st( filts->dims == dims_t(vect_uint32_t{top->dims.dims(1),bot->dims.dims(1),kern_sz.d[1],kern_sz.d[0] },1) );
    assert_st( biases->dims == dims_t(vect_uint32_t{top->dims.dims(1)},1) );
    assert_st( top->dims.dims(1) == cop->u32_param("out_chans") );

    for( uint32_t fix = 0; fix != filts->dims.dims(0); ++fix ) {
      for( uint32_t y = 0; y != top->dims.dims(2); ++y ) {
	for( uint32_t x = 0; x != top->dims.dims(3); ++x ) {
	  float out_pel = 0;
	  i32_pt_t in_ix = u32_to_i32( u32_pt_t{x,y}*cop->stride()) - u32_to_i32(cop->in_pad);
	  for( uint32_t in_chan = 0; in_chan != bot->dims.dims(1); ++in_chan ) {
	    for( uint32_t ky = 0; ky < kern_sz.d[1]; ++ky ) {
	      int32_t in_ky = in_ix.d[1] + ky;
	      if( (in_ky < 0) || (uint32_t(in_ky) >= bot->dims.dims(2)) ) { continue; }
	      for( uint32_t kx = 0; kx < kern_sz.d[0]; ++kx ) {
		int32_t in_kx = in_ix.d[0] + kx;
		if( (in_kx < 0) || (uint32_t(in_kx) >= bot->dims.dims(3)) ) { continue; }
		out_pel += bot->at4( img_ix, in_chan, in_ky, in_kx ) * filts->at4( fix, in_chan, ky, kx );
	      }
	    }
	  }
	  out_pel += biases->at1( fix );
	  top->at4( img_ix, fix, y, x ) = out_pel; // > 0 ? out_pel : 0;
	}
      }
    }
  }

  void run_conv_op_one_img_pool( p_conv_op_t const & cop, p_map_str_p_nda_float_t const & fwd, uint32_t const img_ix, 
				 p_nda_float_t const & bot, p_nda_float_t const & top ) {
    
    u32_pt_t kern_sz = cop->kern_sz;
    if( kern_sz.is_zeros() ) { kern_sz = {bot->dims.dims(3), bot->dims.dims(2)}; } // 'global' input special case
    assert_st( top->dims.dims(1) == bot->dims.dims(1) ); // one-to-one chans IO
    uint32_t const out_pool_elems = kern_sz.dims_prod();
    uint32_t const avg_pool = cop->u32_param("avg_pool");
    for( uint32_t cix = 0; cix != top->dims.dims(1); ++cix ) {
      for( uint32_t y = 0; y != top->dims.dims(2); ++y ) {
	for( uint32_t x = 0; x != top->dims.dims(3); ++x ) {
	  float out_pel = 0;
	  i32_pt_t in_ix = u32_to_i32( u32_pt_t{x,y}*cop->stride()) - u32_to_i32(cop->in_pad);
	  for( uint32_t ky = 0; ky < kern_sz.d[1]; ++ky ) {
	    int32_t in_ky = in_ix.d[1] + ky;
	    if( (in_ky < 0) || (uint32_t(in_ky) >= bot->dims.dims(2)) ) { continue; }
	    for( uint32_t kx = 0; kx < kern_sz.d[0]; ++kx ) {
	      int32_t in_kx = in_ix.d[0] + kx;
	      if( (in_kx < 0) || (uint32_t(in_kx) >= bot->dims.dims(3)) ) { continue; }
	      float const bv = bot->at4( img_ix, cix, in_ky, in_kx );
	      if( avg_pool ) { out_pel += bv; } else { max_eq( out_pel, bv ); }
	    }
	  }
	  if( avg_pool ) { out_pel /= out_pool_elems; }
	  top->at4( img_ix, cix, y, x ) = out_pel; 
	}
      }
    }
  }

  void run_conv_op_per_img( p_conv_op_t const & cop, 
			    p_map_str_p_nda_float_t const & fwd, p_nda_float_t const & bot, p_nda_float_t const & top ) {
    assert( bot->dims.dims(0) == top->dims.dims(0) );
    for( uint32_t i = 0; i != bot->dims.dims(0); ++i ) {
      if( cop->is( Convolution_coi )) { run_conv_op_one_img_conv( cop, fwd, i, bot, top ); }
      else if( cop->is( Pooling_coi )) { run_conv_op_one_img_pool( cop, fwd, i, bot, top ); }
      else { assert_st(0); }
    }
  }

  void run_conv_op( p_conv_op_t const & cop, p_map_str_p_nda_float_t const & fwd ) { 
    if( 0 ) {
    } else if( (cop->is( Convolution_coi )) || (cop->is( Pooling_coi )) ) {
      p_nda_float_t const & bot = must_find( *fwd, cop->bots[0] );
      assert_st( bot->dims.sz() == 4 );
      p_nda_float_t const & top = must_find( *fwd, cop->tops[0] );
      assert_st( top->dims.sz() == 4 );
      assert_st( bot->dims.dims(0) == top->dims.dims(0) );
      run_conv_op_per_img( cop, fwd, bot, top );
    } else if( cop->is( ReLU_coi ) ) {
      p_nda_float_t const & top = must_find( *fwd, cop->get_single_in_place_arg() );
      float * const r_top = &top->elems[0];
      for( uint32_t i = 0; i != top->elems.sz; ++i ) { if( r_top[i] <= 0 ) { r_top[i] = 0; } }
    } else if( cop->is( Dropout_coi ) ) {
      // ingore 
    } else if( cop->is( Softmax_coi ) ) {
      p_nda_float_t const & bot = must_find( *fwd, cop->bots[0] );
      p_nda_float_t const & top = must_find( *fwd, cop->tops[0] );
      assert( bot->dims == top->dims );
      assert( bot->dims.sz() == 4 );
      for( uint32_t i = 0; i != top->dims.dims(0); ++i ) {
	for( uint32_t y = 0; y != top->dims.dims(2); ++y ) {
	  for( uint32_t x = 0; x != top->dims.dims(3); ++x ) {
	    float max_val = std::numeric_limits<float>::lowest(); // get max over chans
	    for( uint32_t c = 0; c != top->dims.dims(1); ++c ) { max_eq( max_val, bot->at4(i,c,y,x) ); } 
	    float sum_val = 0; // get sum(exp()) over chans
	    for( uint32_t c = 0; c != top->dims.dims(1); ++c ) { sum_val += exp(bot->at4(i,c,y,x) - max_val); } 
	    for( uint32_t c = 0; c != top->dims.dims(1); ++c ) { 
	      top->at4(i,c,y,x) = exp(bot->at4(i,c,y,x) - max_val) / sum_val; // normalize
	    } 
	  }
	}
      }
    } else if( cop->is( LRN_coi ) ) {
      p_nda_float_t const & bot = must_find( *fwd, cop->bots[0] );
      p_nda_float_t const & top = must_find( *fwd, cop->tops[0] );
      assert( bot->dims == top->dims );
      assert( bot->dims.sz() == 4 );
      uint32_t const local_size = cop->u32_param("local_size");
      double const alpha = cop->double_param("alpha");
      double const beta = cop->double_param("beta");
      double const k = cop->double_param("k");
      assert( local_size & 1 );
      int32_t const half_win = local_size >> 1;
      for( uint32_t i = 0; i != top->dims.dims(0); ++i ) {
	for( uint32_t y = 0; y != top->dims.dims(2); ++y ) {
	  for( uint32_t x = 0; x != top->dims.dims(3); ++x ) {
	    for( uint32_t c = 0; c != top->dims.dims(1); ++c ) { 
	      float wind_sum_v2 = 0;
	      for( int32_t cw = int32_t(c) - half_win; cw <= int32_t(c) + half_win; ++cw ) {
		if( cw >= 0 && cw < int32_t(top->dims.dims(1)) ) { 
		  float const v = bot->at4(i,cw,y,x); 
		  wind_sum_v2 += v * v; 
		}
	      }
	      top->at4(i,c,y,x) = bot->at4(i,c,y,x) * 
		powf((k + alpha*wind_sum_v2/float(local_size)), -beta);
	    } 
	  }
	}
      }
    } else if( cop->is( Concat_coi ) ) {
      p_nda_float_t const & top = must_find( *fwd, cop->tops[0] );
      assert( top->dims.sz() == 4 );
      uint32_t chans_out_done = 0;
      for( uint32_t bi = 0; bi != cop->bots.size(); ++bi ) {
	p_nda_float_t const & bot = must_find( *fwd, cop->bots[bi] );
	assert( bot->dims.sz() == 4 );
	assert( bot->dims.dims(0) == top->dims.dims(0) );
	assert( bot->dims.dims(2) == top->dims.dims(2) );
	assert( bot->dims.dims(3) == top->dims.dims(3) );
	assert_st( chans_out_done+bot->dims.dims(1) <= top->dims.dims(1) );
	for( uint32_t i = 0; i != bot->dims.dims(0); ++i ) {
	  for( uint32_t c = 0; c != bot->dims.dims(1); ++c ) { 
	    for( uint32_t y = 0; y != bot->dims.dims(2); ++y ) {
	      for( uint32_t x = 0; x != bot->dims.dims(3); ++x ) {
		top->at4(i,chans_out_done+c,y,x) = bot->at4(i,c,y,x);
	      } 
	    }
	  }
	}
	chans_out_done += bot->dims.dims(1);
      }
      assert_st( chans_out_done == top->dims.dims(1) );
    } else { rt_err( "unhandled operation: " + cop->type ); }
  }

  void run_ops_rec( p_conv_pipe_t const & cp,  p_map_str_p_nda_float_t const & fwd, string const & node_name ) {
    p_conv_node_t const & node = cp->must_get_node( node_name );
    for( vect_p_conv_op_t::const_iterator j = node->in_place_ops.begin(); j != node->in_place_ops.end(); ++j ) { run_conv_op( *j, fwd ); }
    for( vect_string::const_iterator i = node->bot_for.begin(); i != node->bot_for.end(); ++i ) {
      p_conv_op_t const & cop = cp->get_op( *i );
      if( !cop->on_seen_bot() ) { continue; } // wait till we've seen all bottoms
      run_conv_op( cop, fwd );
      for( vect_string::const_iterator i = cop->tops.begin(); i != cop->tops.end(); ++i ) { run_ops_rec( cp, fwd, *i ); }
    }
  }

  void slow_cpu_fwd_t::run_fwd( vect_string const & to_set_vns, p_map_str_p_nda_float_t const & fwd, vect_string const & to_get_vns ) {
    timer_t t("slow_cpu_fwd_t::run_fwd");
    for( vect_string::const_iterator i = to_set_vns.begin(); i != to_set_vns.end(); ++i ) {
      if( !has( *fwd, *i ) ) { rt_err( "slow_cpu_fwd: requested input (to_set) var '"+*i+"' was not present in fwd at start of run_fwd()." ); }
    }
    cp->fwd_alloc_ndas( fwd, 0 );
    if( skip_work ) { return; }
    cp->topo_visit_setup();
    for( set_string::const_iterator i = cp->bots.begin(); i != cp->bots.end(); ++i ) { run_ops_rec( cp, fwd, *i ); }
    for( vect_string::const_iterator i = to_get_vns.begin(); i != to_get_vns.end(); ++i ) {
      if( !has( *fwd, *i ) ) { rt_err( "slow_cpu_fwd: requested output (to_get) var '"+*i+"' was not computed." ); }
    }
  }  
#include"gen/slow_cpu_fwd.cc.nesi_gen.cc"
}
