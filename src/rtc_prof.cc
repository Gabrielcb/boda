// Copyright (c) 2015, Matthew W. Moskewicz <moskewcz@alumni.princeton.edu>; part of Boda framework; see LICENSE
#include"boda_tu_base.H"
#include"timers.H"
#include<boost/filesystem.hpp>
#include<boost/lexical_cast.hpp>
#include"lexp.H"
#include"has_main.H"
#include"rtc_func_gen.H"
#include"rtc_compute.H"

namespace boda 
{
  typedef shared_ptr< dims_t > p_dims_t; 

  struct rtc_prof_t : virtual public nesi, public has_main_t // NESI(help="profile set of rtc functions",
		      // bases=["has_main_t"], type_id="rtc_prof" )

  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support

    uint32_t show_compile_log; //NESI(default=0,help="if 1, print compilation log")
    uint32_t show_rtc_calls; //NESI(default=0,help="if 1, print rtc calls")
    uint32_t enable_lineinfo; //NESI(default=0,help="if 1, enable lineinfo for ptx compilation")
    uint32_t show_func_attrs; //NESI(default=0,help="if 1, print func attrs after load")

    filename_t rtc_func_sigs_fn; //NESI(default="%(boda_test_dir)/rtc_func_sigs.txt",help="file to hold all generated func signatures")
    p_dims_t dummy_dims; // NESI(help="HACK: dummy NESI var of type dims_t (otherwise unused) to force tinfo generation. see map_str_T FIXME in nesi.cc")

    p_rtc_compute_t rtc; //NESI(default="(be=nvrtc)",help="rtc back-end to use")
    string per_call_fn; //NESI(default="",help="if non-empty, write per-call profiling (timing via events) to given file.")

    vect_rcg_func_call_t calls;
    // rtc->create_var_with_dims_floats( name, cp->must_get_node(node_name)->dims );
    // calls.push_back( rcg_func_call_t{ gen_fn, oi->tag, oi->arg_map } );
    
    rtc_codegen_t codegen;

    virtual void main( nesi_init_arg_t * nia );

    string gen_func( rtc_func_sig_t const & rfs );
    void run_rfc( rcg_func_call_t & rfc );
    void run_calls( void );
  };
 
  string rtc_prof_t::gen_func( rtc_func_sig_t const & rfs ) { 
    p_custom_codegen_t ccc = make_cnn_custom_codegen_t();
    return codegen.gen_func( ccc.get(), rfs ); 
  }

  void rtc_prof_t::main( nesi_init_arg_t * nia ) {
    rtc->init();
    codegen.read_rtc_func_sigs( rtc_func_sigs_fn );
#if 0
    if( enable_bwai_test ) { // test bwai gen
      assert_st(0);
      rtc->create_var_with_dims_floats( "a", dims_t{ {1000,1024}, {"M","K"}, 1 } );
      rtc->create_var_with_dims_floats( "b", dims_t{ {1000,1024}, {"N","K"}, 1 } );
      rtc->create_var_with_dims_floats( "c", dims_t{ {1000,1000}, {"M","N"}, 1 } );
      map_str_dims_t bwai_ref_dims;
      bwai_ref_dims["work"] = dims_t{ {10,10,10,10,32,10,10}, {"Mg","Ng","Mb","Nb","Kb","Mt","Nt"}, 1 };
      gen_call( "bwai", map_str_str(), "bwai_sgemm", {"a","b","c"}, bwai_ref_dims, 0 );
    }
#endif
    
    rtc->compile( codegen.rtc_prog_str, show_compile_log, enable_lineinfo );
    for( rtc_func_names_map_t::iterator i = codegen.rtc_func_names_map.begin(); i != codegen.rtc_func_names_map.end(); ++i ) { rtc->check_runnable( i->first, show_func_attrs ); }

    rtc->finish_and_sync();
    run_calls();
  }

  void rtc_prof_t::run_rfc( rcg_func_call_t & rfc ) { codegen.run_rfc( rtc, show_rtc_calls, rfc, 0 );  }

  void rtc_prof_t::run_calls( void ) {
    timer_t t("rtc_prof_t::run_fwd");
    bool const enable_prof = 0;
    if( enable_prof ) { rtc->profile_start(); }
    for( vect_rcg_func_call_t::iterator i = calls.begin(); i != calls.end(); ++i ) { run_rfc( *i ); }
    rtc->finish_and_sync();
    float const compute_dur = calls.empty() ? 0.0f : rtc->get_dur( calls.front().call_id, calls.back().call_id );
    if( enable_prof ) { rtc->profile_stop(); }
    if( !per_call_fn.empty() ) {
      p_ofstream out = ofs_open( per_call_fn );
      (*out) << strprintf("net.args.runtime=%s\n", str(compute_dur/1000.0).c_str() );
      for( vect_rcg_func_call_t::iterator i = calls.begin(); i != calls.end(); ++i ) {
	rcg_func_call_t & rfc = *i;
	if( rfc.call_tag.empty() ) { continue; }
	float const rfc_dur = rtc->get_dur( rfc.call_id, rfc.call_id );
	(*out) << strprintf( "per_layer_time['%s']=per_layer_time.get('%s',0.0) + %s # %s \n", 
			     str(rfc.call_tag).c_str(), str(rfc.call_tag).c_str(), str(rfc_dur/1000.0).c_str(), rfc.rtc_func_name.c_str() );
      }
    }
  }
#include"gen/rtc_prof.cc.nesi_gen.cc"

}
