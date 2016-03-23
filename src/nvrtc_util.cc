// Copyright (c) 2015, Matthew W. Moskewicz <moskewcz@alumni.princeton.edu>; part of Boda framework; see LICENSE
#include"boda_tu_base.H"
#include"nvrtc_util.H"
#include"str_util.H"
#include"timers.H"
#include<nvrtc.h>
#include<cuda.h>
#include<cudaProfiler.h>
#include"rtc_compute.H"

namespace boda 
{

  void nvrtc_err_chk( nvrtcResult const & ret, char const * const func_name ) {
    if( ret != NVRTC_SUCCESS ) { rt_err( strprintf( "%s() failed with ret=%s (%s)", func_name, str(ret).c_str(), nvrtcGetErrorString(ret) ) ); } }
  void nvrtcDestroyProgram_wrap( nvrtcProgram p ) { if(!p){return;} nvrtc_err_chk( nvrtcDestroyProgram( &p ), "nvrtcDestroyProgram" ); }
  typedef shared_ptr< _nvrtcProgram > p_nvrtcProgram;

  void cu_err_chk( CUresult const & ret, char const * const func_name ) {
    if( ret != CUDA_SUCCESS ) { 
      char const * ret_name;
      char const * ret_str;
      assert_st( cuGetErrorName( ret, &ret_name ) == CUDA_SUCCESS );
      assert_st( cuGetErrorString( ret, &ret_str ) == CUDA_SUCCESS );
      rt_err( strprintf( "%s() failed with ret=%s (%s)", func_name, ret_name, ret_str ) );
    }
  }
  
  p_nvrtcProgram make_p_nvrtcProgram( string const & cuda_prog_str ) { 
    nvrtcProgram p;
    nvrtc_err_chk( nvrtcCreateProgram( &p, &cuda_prog_str[0], "boda_cuda_gen", 0, 0, 0 ), "nvrtcCreateProgram" );
    return p_nvrtcProgram( p, nvrtcDestroyProgram_wrap ); 
  }
  string nvrtc_get_compile_log( p_nvrtcProgram const & cuda_prog ) {
    string ret;
    size_t ret_sz = 0;
    nvrtc_err_chk( nvrtcGetProgramLogSize( cuda_prog.get(), &ret_sz ), "nvrtcGetProgramLogSize" );
    ret.resize( ret_sz );    
    nvrtc_err_chk( nvrtcGetProgramLog( cuda_prog.get(), &ret[0] ), "nvrtcGetProgramLog" );
    return ret;
  }
  string nvrtc_get_ptx( p_nvrtcProgram const & cuda_prog ) {
    string ret;
    size_t ret_sz = 0;
    nvrtc_err_chk( nvrtcGetPTXSize( cuda_prog.get(), &ret_sz ), "nvrtcGetPTXSize" );
    ret.resize( ret_sz );    
    nvrtc_err_chk( nvrtcGetPTX( cuda_prog.get(), &ret[0] ), "nvrtcGetPTX" );
    return ret;
  }

  // FIXME: add function to get SASS? can use this command sequence:
  // ptxas out.ptx -arch sm_52 -o out.cubin ; nvdisasm out.cubin > out.sass

  string nvrtc_compile( string const & cuda_prog_str, bool const & print_log, bool const & enable_lineinfo ) {
    timer_t t("nvrtc_compile");
    p_nvrtcProgram cuda_prog = make_p_nvrtcProgram( cuda_prog_str );
    vect_string cc_opts = {"--use_fast_math",
			   "--gpu-architecture=compute_52",
			   "--restrict"};
    if( enable_lineinfo ) { cc_opts.push_back("-lineinfo"); }
    auto const comp_ret = nvrtcCompileProgram( cuda_prog.get(), cc_opts.size(), &get_vect_rp_const_char( cc_opts )[0] );
    string const log = nvrtc_get_compile_log( cuda_prog );
    if( print_log ) { printf( "NVRTC COMPILE LOG:\n%s\n", str(log).c_str() ); }
    nvrtc_err_chk( comp_ret, ("nvrtcCompileProgram\n"+log).c_str() ); // delay error check until after getting log
    return nvrtc_get_ptx( cuda_prog );
  }

#ifdef CU_GET_FUNC_ATTR_HELPER_MACRO
#error
#endif
#define CU_GET_FUNC_ATTR_HELPER_MACRO( cf, attr ) cu_get_func_attr( cf, attr, #attr )  
  string cu_get_func_attr( CUfunction const & cf, CUfunction_attribute const & cfa, char const * const & cfa_str ) {
    int cfav = 0;
    cu_err_chk( cuFuncGetAttribute( &cfav, cfa, cf ), "cuFuncGetAttribute" );
    return strprintf( "  %s=%s\n", str(cfa_str).c_str(), str(cfav).c_str() );
  }
  string cu_get_all_func_attrs( CUfunction const & cf ) {
    string ret;
    ret += CU_GET_FUNC_ATTR_HELPER_MACRO( cf, CU_FUNC_ATTRIBUTE_MAX_THREADS_PER_BLOCK );
    ret += CU_GET_FUNC_ATTR_HELPER_MACRO( cf, CU_FUNC_ATTRIBUTE_SHARED_SIZE_BYTES );
    ret += CU_GET_FUNC_ATTR_HELPER_MACRO( cf, CU_FUNC_ATTRIBUTE_CONST_SIZE_BYTES );
    ret += CU_GET_FUNC_ATTR_HELPER_MACRO( cf, CU_FUNC_ATTRIBUTE_LOCAL_SIZE_BYTES );
    ret += CU_GET_FUNC_ATTR_HELPER_MACRO( cf, CU_FUNC_ATTRIBUTE_NUM_REGS );
    ret += CU_GET_FUNC_ATTR_HELPER_MACRO( cf, CU_FUNC_ATTRIBUTE_PTX_VERSION );
    ret += CU_GET_FUNC_ATTR_HELPER_MACRO( cf, CU_FUNC_ATTRIBUTE_BINARY_VERSION );
    ret += CU_GET_FUNC_ATTR_HELPER_MACRO( cf, CU_FUNC_ATTRIBUTE_CACHE_MODE_CA );
    return ret;
  }
#undef CU_GET_FUNC_ATTR_HELPER_MACRO

  
  template< typename T >  struct cup_T {
    typedef T element_type;
    CUdeviceptr p;
    uint32_t sz;
    void set_to_zero( void ) { cu_err_chk( cuMemsetD8(  p, 0, sz * sizeof(element_type) ), "cuMemsetD8" ); }
    cup_T( uint32_t const sz_ ) : p(0), sz(sz_) { 
      cu_err_chk( cuMemAlloc( &p,    sz * sizeof(element_type) ), "cuMemAlloc" ); 
      set_to_zero();
    }
    ~cup_T( void ) { cu_err_chk( cuMemFree( p ), "cuMemFree" ); }
  };
  typedef cup_T< float > cup_float;
  typedef shared_ptr< cup_float > p_cup_float; 

  typedef map< string, p_cup_float > map_str_p_cup_float_t;
  typedef shared_ptr< map_str_p_cup_float_t > p_map_str_p_cup_float_t;

  typedef shared_ptr< CUevent > p_CUevent; 
  typedef vector< p_CUevent > vect_p_CUevent; 
  void cuEventDestroy_wrap( CUevent const * const p ) { 
    if(!p){return;} 
    cu_err_chk( cuEventDestroy( *p ), "cuEventDestroy" ); 
  }
  p_CUevent make_p_CUevent( void ) {
    CUevent ret;
    cu_err_chk( cuEventCreate( &ret, CU_EVENT_DEFAULT ), "cuEventCreate" );
    return p_CUevent( new CUevent( ret ), cuEventDestroy_wrap ); 
  }

  typedef shared_ptr< CUmodule > p_CUmodule; 
  void cuModuleUnload_wrap( CUmodule const * const p ) { if(!p){return;}  cu_err_chk( cuModuleUnload( *p ), "cuModuleUnload" ); }
  p_CUmodule make_p_CUmodule( CUmodule to_own ) { return p_CUmodule( new CUmodule( to_own ), cuModuleUnload_wrap ); }

  // unlink opencl, functions implicity tied to the lifetime of thier module. if we use one-module-per-function, we need
  // to keep a ref to the module alongside the func so we can free the module when we want to free the function. also,
  // note that there is no reference counting for the module in the CUDA driver API, just load/unload, so we'd need to
  // do that ourselves. this might not be needed currently, but allows for arbitrary funcs->modules mappings.
  struct nv_func_info_t {
    CUfunction func;
    p_CUmodule mod;
  };

  typedef map< string, nv_func_info_t > map_str_nv_func_info_t;
  typedef shared_ptr< map_str_nv_func_info_t > p_map_str_nv_func_info_t; 

  struct call_ev_t {
    p_CUevent b_ev;
    p_CUevent e_ev;
    call_ev_t( void ) : b_ev( make_p_CUevent() ), e_ev( make_p_CUevent() ) { }
  };
  typedef vector< call_ev_t > vect_call_ev_t; 

  struct var_info_t {
    p_cup_float cup;
    dims_t dims;
    //p_void ev; // when ready
    var_info_t( dims_t const & dims_ ) : cup( make_shared<cup_float>( dims_.dims_prod() ) ), dims(dims_) {} // , ev( make_p_CUevent() ) { }
  };

  typedef map< string, var_info_t > map_str_var_info_t;
  typedef shared_ptr< map_str_var_info_t > p_map_str_var_info_t;

  string cu_base_decls = R"rstr(
typedef unsigned uint32_t;
uint32_t const U32_MAX = 0xffffffffU;
typedef int int32_t;
//typedef long long int64_t;
float const FLT_MAX = /*0x1.fffffep127f*/ 340282346638528859811704183484516925440.0f;
float const FLT_MIN = 1.175494350822287507969e-38f;
#define CUCL_GLOBAL_KERNEL extern "C" __global__
#define GASQ
#define GLOB_ID_1D (blockDim.x * blockIdx.x + threadIdx.x)
#define LOC_ID_1D (threadIdx.x)
#define GRP_ID_1D (blockIdx.x)
#define LOC_SZ_1D (blockDim.x)
#define LOCSHAR_MEM __shared__
#define LSMASQ
#define BARRIER_SYNC __syncthreads()
)rstr";

  struct nvrtc_compute_t : virtual public nesi, public rtc_compute_t // NESI(help="libnvrtc based rtc support (i.e. CUDA)",
			   // bases=["rtc_compute_t"], type_id="nvrtc" )
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support
    // FIXME: can/should we init these cu_* vars?
    CUdevice cu_dev;
    CUcontext cu_context;
    zi_bool init_done;
    void init( void ) {
      assert_st( !init_done.v );
      null_cup = 0;

      cu_err_chk( cuInit( 0 ), "cuInit" ); 
      cu_err_chk( cuDeviceGet( &cu_dev, 0 ), "cuDeviceGet" );
      //cu_err_chk( cuCtxCreate( &cu_context, 0, cu_dev ), "cuCtxCreate" );
      cu_err_chk( cuDevicePrimaryCtxRetain( &cu_context, cu_dev ), "cuDevicePrimaryCtxRetain" );
      cu_err_chk( cuCtxSetCurrent( cu_context ), "cuCtxSetCurrent" ); // is this always needed/okay?
      // cu_err_chk( cuCtxSetCacheConfig( CU_FUNC_CACHE_PREFER_L1 ), "cuCtxSetCacheConfig" ); // does nothing?

      init_done.v = 1;
    }

    p_CUmodule cu_mod;
    void compile( string const & cucl_src, bool const show_compile_log, bool const enable_lineinfo ) {
      string const src = cu_base_decls + cucl_src;
      assert( init_done.v );
      write_whole_fn( "out.cu", src );
      string const prog_ptx = nvrtc_compile( src, show_compile_log, enable_lineinfo );
      write_whole_fn( "out.ptx", prog_ptx );
      assert( !cu_mod );
      CUmodule new_cu_mod;
      cu_err_chk( cuModuleLoadDataEx( &new_cu_mod, prog_ptx.c_str(), 0, 0, 0 ), "cuModuleLoadDataEx" );
      cu_mod = make_p_CUmodule( new_cu_mod );
    }
    CUdeviceptr null_cup; // inited to 0; used to pass null device pointers to kernels. note, however, that the value is
			  // generally unused, so the value doesn't really matter currently. it might later of course.
    p_map_str_var_info_t vis;
    p_map_str_nv_func_info_t cu_funcs;

    vect_call_ev_t call_evs;
    call_ev_t & get_call_ev( uint32_t const & call_id ) { assert_st( call_id < call_evs.size() ); return call_evs[call_id]; }
    uint32_t alloc_call_id( void ) { call_evs.push_back( call_ev_t() ); return call_evs.size() - 1; }
    virtual void release_per_call_id_data( void ) { call_evs.clear(); } // invalidates all call_ids inside rtc_func_call_t's

    virtual float get_dur( uint32_t const & b, uint32_t const & e ) {
      float compute_dur = 0.0f;
      cu_err_chk( cuEventElapsedTime( &compute_dur, *get_call_ev(b).b_ev, *get_call_ev(e).e_ev ), "cuEventElapsedTime" );
      return compute_dur;
    }

    void copy_to_var( string const & vn, float const * const v ) {
      var_info_t const & vi = must_find( *vis, vn );
      cu_err_chk( cuMemcpyHtoDAsync( vi.cup->p, v, vi.cup->sz*sizeof(float), 0 ), "cuMemcpyHtoD" );
      //record_event( vi.ev );
    }
    void copy_from_var( float * const v, string const & vn ) {
      p_cup_float const & cup = must_find( *vis, vn ).cup;
      cu_err_chk( cuMemcpyDtoH( v, cup->p, cup->sz*sizeof(float) ), "cuMemcpyDtoH" );
    }
    void create_var_with_dims_floats( string const & vn, dims_t const & dims ) { 
      must_insert( *vis, vn, var_info_t( dims ) ); 
    }
    dims_t get_var_dims_floats( string const & vn ) { return must_find( *vis, vn ).dims; }
    void set_var_to_zero( string const & vn ) { must_find( *vis, vn ).cup->set_to_zero(); }
    
    nvrtc_compute_t( void ) : vis( new map_str_var_info_t ), cu_funcs( new map_str_nv_func_info_t ) { }

    // note: post-compilation, MUST be called exactly once on all functions that will later be run()
    void check_runnable( string const name, bool const show_func_attrs ) {
      assert_st( cu_mod );
      CUfunction cu_func;
      cu_err_chk( cuModuleGetFunction( &cu_func, *cu_mod, name.c_str() ), "cuModuleGetFunction" );
      // FIXME: i'd like to play with enabling L1 caching for these kernels, but it's not clear how to do that
      // cu_err_chk( cuFuncSetCacheConfig( cu_func, CU_FUNC_CACHE_PREFER_L1 ), "cuFuncSetCacheConfig" ); // does nothing?
      if( show_func_attrs ) {
	string rfas = cu_get_all_func_attrs( cu_func );
	printf( "%s: \n%s", name.c_str(), str(rfas).c_str() );
      }
      must_insert( *cu_funcs, name, nv_func_info_t{ cu_func, cu_mod } );
    }

    void add_args( vect_string const & args, vect_rp_void & cu_func_args ) {
      for( vect_string::const_iterator i = args.begin(); i != args.end(); ++i ) {
	if( *i == "<NULL>" ) { 
	  cu_func_args.push_back( &null_cup );
	} else {
	  p_cup_float const & cu_v = must_find( *vis, *i ).cup;
	  cu_func_args.push_back( &cu_v->p );
	}
      }
    }
#if 0
    void record_var_events( vect_string const & vars, rtc_func_call_t const & rfc ) {
      for( vect_string::const_iterator i = vars.begin(); i != vars.end(); ++i ) { must_find( *vis, *i ).ev = rfc.e_ev; }
    }
#endif
    void run( rtc_func_call_t & rfc ) {
      CUfunction & cu_func = must_find( *cu_funcs, rfc.rtc_func_name.c_str() ).func;
      vect_rp_void cu_func_args;
      add_args( rfc.in_args, cu_func_args );
      add_args( rfc.inout_args, cu_func_args );
      add_args( rfc.out_args, cu_func_args );
      for( vect_uint32_t::iterator i = rfc.u32_args.begin(); i != rfc.u32_args.end(); ++i ) { cu_func_args.push_back( &(*i) ); }
      rfc.call_id = alloc_call_id();

      timer_t t("cu_launch_and_sync");
      record_event( get_call_ev(rfc.call_id).b_ev );
      cu_err_chk( cuLaunchKernel( cu_func,
				  rfc.blks.v, 1, 1, // grid x,y,z dims
				  rfc.tpb.v, 1, 1, // block x,y,z dims
				  0, 0, // smem_bytes, stream_ix
				  &cu_func_args[0], // cu_func's args
				  0 ), "cuLaunchKernel" ); // unused 'extra' arg-passing arg
      record_event( get_call_ev(rfc.call_id).e_ev );
      //record_var_events( rfc.inout_args, rfc );
      //record_var_events( rfc.out_args, rfc );
    }

    void finish_and_sync( void ) { cu_err_chk( cuCtxSynchronize(), "cuCtxSynchronize" ); }

    void profile_start( void ) { cuProfilerStart(); }
    void profile_stop( void ) { cuProfilerStop(); }

  protected:
    void record_event( p_void const & ev ) { cu_err_chk( cuEventRecord( *(CUevent*)ev.get(), 0 ), "cuEventRecord" ); }

  };
  struct nvrtc_compute_t; typedef shared_ptr< nvrtc_compute_t > p_nvrtc_compute_t; 

#include"gen/nvrtc_util.cc.nesi_gen.cc"
}
