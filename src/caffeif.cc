// Copyright (c) 2013-2014, Matthew W. Moskewicz <moskewcz@alumni.princeton.edu>; part of Boda framework; see LICENSE
#include"boda_tu_base.H"
#include"caffepb.H"
#include"timers.H"
#include"str_util.H"
#include"img_io.H"
#include"lexp.H"
#include"nesi.H"
#include"conv_util.H"
#include"caffeif.H"
//#include<glog/logging.h>
#include<google/protobuf/text_format.h>
//#include"caffe/proto/caffe.pb.h"
#include"caffe/caffe.hpp"
#include"anno_util.H"
#include"rand_util.H"

namespace boda 
{
  using caffe::Caffe;
  using caffe::Blob;

  std::ostream & operator <<(std::ostream & os, scale_info_t const & v ) { 
    return os << strprintf( "from_upsamp_net=%s bix=%s feat_box=%s feat_img_box=%s\n", 
			    str(v.from_upsamp_net).c_str(), str(v.bix).c_str(), str(v.feat_box).c_str(), 
			    str(v.feat_img_box).c_str() ); 
  }
  
  void subtract_mean_and_copy_img_to_batch( p_nda_float_t const & in_batch, uint32_t img_ix, p_img_t const & img ) {
    timer_t t("subtract_mean_and_copy_img_to_batch");
    dims_t const & ibd = in_batch->dims;
    assert_st( img_ix < ibd.dims(0) );
    assert_st( 3 == ibd.dims(1) );
    assert_st( img->sz.d[0] == ibd.dims(3) );
    assert_st( img->sz.d[1] == ibd.dims(2) );
    uint32_t const inmc = 123U+(117U<<8)+(104U<<16)+(255U<<24); // RGBA
#pragma omp parallel for	  
    for( uint32_t y = 0; y < ibd.dims(2); ++y ) {
      for( uint32_t x = 0; x < ibd.dims(3); ++x ) {
	uint32_t const pel = img->get_pel({x,y});
	for( uint32_t c = 0; c < 3; ++c ) {
	  // note: RGB -> BGR swap via the '2-c' below
	  in_batch->at4( img_ix, 2-c, y, x ) = get_chan(c,pel) - float(uint8_t(inmc >> (c*8)));
	}
      }
    }
  }

  void chans_to_area( uint32_t & out_s, u32_pt_t & out_sz, u32_pt_t const & in_sz, uint32_t in_chan ) {
    out_s = u32_ceil_sqrt( in_chan );
    out_sz = in_sz.scale( out_s );
  }

  void copy_batch_to_img( p_nda_float_t const & out_batch, uint32_t img_ix, p_img_t const & img, u32_box_t region ) {
    if( region == u32_box_t{} ) { region.p[1] = u32_pt_t{out_batch->dims.dims(3),out_batch->dims.dims(2)}; }    
    // set up dim iterators that span only the image we want to process
    dims_t img_e( out_batch->dims.sz() );
    dims_t img_b( img_e.sz() );
    img_b.dims(0) = img_ix;
    img_e.dims(0) = img_ix + 1;
    img_b.dims(1) = 0;
    img_e.dims(1) = out_batch->dims.dims(1);
    img_b.dims(2) = region.p[0].d[1];
    img_e.dims(2) = region.p[1].d[1];
    img_b.dims(3) = region.p[0].d[0];
    img_e.dims(3) = region.p[1].d[0];
    float const out_max = nda_reduce( *out_batch, max_functor<float>(), 0.0f, img_b, img_e ); // note clamp to 0
    //float const out_min = nda_reduce( *out_batch, min_functor<float>(), 0.0f, img_b, img_e ); // note clamp to 0
    //assert_st( out_min == 0.0f ); // shouldn't be any negative values
    //float const out_rng = out_max - out_min;
    copy_batch_to_img( out_batch, img_ix, img, region, out_max );
  }
  void copy_batch_to_img( p_nda_float_t const & out_batch, uint32_t img_ix, p_img_t const & img, u32_box_t region, 
			  float const & out_max ) {
    if( region == u32_box_t{} ) { region.p[1] = u32_pt_t{out_batch->dims.dims(3),out_batch->dims.dims(2)}; }    
    dims_t const & obd = out_batch->dims;
    assert( obd.sz() == 4 );
    assert_st( img_ix < obd.dims(0) );

    u32_pt_t const out_sz( obd.dims(3), obd.dims(2) );
    uint32_t sqrt_out_chan;
    u32_pt_t img_sz;
    chans_to_area( sqrt_out_chan, img_sz, out_sz, obd.dims(1) );
    assert( sqrt_out_chan );
    assert( (sqrt_out_chan*sqrt_out_chan) >= obd.dims(1) );

    assert_st( img->sz == img_sz );

    for( uint32_t by = region.p[0].d[1]; by < region.p[1].d[1]; ++by ) {
      for( uint32_t bx = region.p[0].d[0]; bx < region.p[1].d[0]; ++bx ) {
	for( uint32_t bc = 0; bc < obd.dims(1); ++bc ) {
	  uint32_t const x = bx*sqrt_out_chan + (bc%sqrt_out_chan);
	  uint32_t const y = by*sqrt_out_chan + (bc/sqrt_out_chan);
	  //float const norm_val = ((out_batch->at4(img_ix,bc,by,bx)-out_min) / out_rng );
	  float const norm_val = ((out_batch->at4(img_ix,bc,by,bx)) / out_max );
	  uint32_t const gv = grey_to_pel( uint8_t( std::min( 255.0, 255.0 * norm_val ) ) );
	  //gv =grey_to_pel( uint8_t( std::min( 255.0, 255.0 * (log(.01) - log(std::max(.01f,norm_val))) / (-log(.01)) )));
	  img->set_pel( {x, y}, gv );
	}
      }
    }
  }

  void init_caffe( uint32_t const gpu_id ) {
    static bool caffe_is_init = 0;
    if( !caffe_is_init ) {
      caffe_is_init = 1;
      timer_t t("caffe_init");
      google::InitGoogleLogging("boda_caffe");
      Caffe::set_mode(Caffe::GPU);
      Caffe::SetDevice(gpu_id);
      //Caffe::set_mode(Caffe::CPU);
    }
  }

  p_Net_float caffe_create_net( net_param_t & net_param, string const & trained_fn ) {
    // for now, we mimic the behavior of the caffe Net ctor that takes
    // a phase and 'force' the phase in the passed net. hey, it is
    // passed by non-const ref, right?
    net_param.mutable_state()->set_phase(caffe::TEST);
    p_Net_float net( new Net_float( net_param ) );
    net->CopyTrainedLayersFrom( trained_fn );
    return net;
  }

  void raw_do_forward( p_Net_float net_, vect_p_nda_float_t const & bottom ) {
    timer_t t("caffe_forward");
    vector<caffe::Blob<float>*> const & input_blobs = net_->input_blobs();
    assert_st( bottom.size() == input_blobs.size() );
    for (unsigned int i = 0; i < input_blobs.size(); ++i) {
      assert_st( bottom[i]->elems.sz == uint32_t(input_blobs[i]->count()) );
      const float* const data_ptr = &bottom[i]->elems[0];
      switch ( Caffe::mode() ) {
      case Caffe::CPU:
	memcpy(input_blobs[i]->mutable_cpu_data(), data_ptr,
	       sizeof(float) * input_blobs[i]->count());
	break;
      case Caffe::GPU:
	cudaMemcpy(input_blobs[i]->mutable_gpu_data(), data_ptr,
		   sizeof(float) * input_blobs[i]->count(), cudaMemcpyHostToDevice);
	break;
      default:
	rt_err( "Unknown Caffe mode." );
      }  // switch (Caffe::mode())
    }
    //const vector<Blob<float>*>& output_blobs = net_->ForwardPrefilled();
    net_->ForwardPrefilled();
  }

  uint32_t get_layer_ix( p_Net_float net_, string const & out_layer_name ) {
    vect_string const & layer_names = net_->layer_names();
    for( uint32_t i = 0; i != layer_names.size(); ++i ) { if( out_layer_name == layer_names[i] ) { return i; } }
    rt_err( strprintf("layer out_layer_name=%s not found in network\n",str(out_layer_name).c_str() )); 
  }

  void copy_output_blob_data( p_Net_float net_, string const & out_layer_name, vect_p_nda_float_t & top ) {
    timer_t t("caffe_copy_output_blob_data");
    uint32_t const out_layer_ix = get_layer_ix( net_, out_layer_name );
    const vector<Blob<float>*>& output_blobs = net_->top_vecs()[ out_layer_ix ];
    top.clear();
    for( uint32_t bix = 0; bix < output_blobs.size(); ++bix ) {
      Blob<float> * const output_blob = output_blobs[bix];
      dims_t out_batch_dims( 4 );
      out_batch_dims.dims(3) = output_blob->width();
      out_batch_dims.dims(2) = output_blob->height();
      out_batch_dims.dims(1) = output_blob->channels();
      out_batch_dims.dims(0) = output_blob->num();
      p_nda_float_t out_batch( new nda_float_t( out_batch_dims ) );
      assert_st( out_batch->elems.sz == uint32_t(output_blob->count()) );
      top.push_back( out_batch );
      
      float * const dest = &out_batch->elems[0];
      switch (Caffe::mode()) {
      case Caffe::CPU: memcpy(dest, output_blob->cpu_data(), sizeof(float) * output_blob->count() ); break;
      case Caffe::GPU: cudaMemcpy(dest, output_blob->gpu_data(), sizeof(float) * output_blob->count(), 
				  cudaMemcpyDeviceToHost); break;
      default: LOG(FATAL) << "Unknown Caffe mode.";
      }  // switch (Caffe::mode())
    }
  }


  void copy_layer_blobs( p_Net_float net_, uint32_t const & layer_ix, vect_p_nda_float_t & blobs ) {
    timer_t t("caffe_copy_layer_blob_data");
    caffe::Layer<float>* layer = net_->layers().at( layer_ix ).get();
    const vector< shared_ptr< caffe::Blob<float> > >& layer_blobs = layer->blobs();
    blobs.clear();
    for( uint32_t bix = 0; bix < layer_blobs.size(); ++bix ) {
      Blob<float> * const layer_blob = layer_blobs[bix].get();
      dims_t blob_dims( 4 );
      blob_dims.dims(3) = layer_blob->width();
      blob_dims.dims(2) = layer_blob->height();
      blob_dims.dims(1) = layer_blob->channels();
      blob_dims.dims(0) = layer_blob->num();
      p_nda_float_t blob( new nda_float_t( blob_dims ) );
      assert_st( blob->elems.sz == uint32_t(layer_blob->count()) );
      blobs.push_back( blob );
      float * const dest = &blob->elems[0];
      switch (Caffe::mode()) {
      case Caffe::CPU: memcpy(dest, layer_blob->cpu_data(), sizeof(float) * layer_blob->count() ); break;
      case Caffe::GPU: cudaMemcpy(dest, layer_blob->gpu_data(), sizeof(float) * layer_blob->count(), 
				  cudaMemcpyDeviceToHost); break;
      default: LOG(FATAL) << "Unknown Caffe mode.";
      }  // switch (Caffe::mode())
    }
  }
  void copy_layer_blobs( p_Net_float net_, string const & layer_name, vect_p_nda_float_t & blobs ) {
    uint32_t const layer_ix = get_layer_ix( net_, layer_name );
    copy_layer_blobs( net_, layer_ix, blobs );
  }

  void set_layer_blobs( p_Net_float net_, uint32_t const & layer_ix, vect_p_nda_float_t & blobs ) {
    timer_t t("caffe_set_layer_blob_data");
    caffe::Layer<float>* layer = net_->layers()[ layer_ix ].get();
    const vector< shared_ptr< caffe::Blob<float> > >& layer_blobs = layer->blobs();
    assert( blobs.size() == layer_blobs.size() );
    for( uint32_t bix = 0; bix < layer_blobs.size(); ++bix ) {
      p_nda_float_t const & blob = blobs[bix];
      Blob<float> * const layer_blob = layer_blobs[bix].get();
      dims_t const & blob_dims = blob->dims;
      assert_st( blob_dims.dims(3) == (uint32_t)layer_blob->width() );
      assert_st( blob_dims.dims(2) == (uint32_t)layer_blob->height() );
      assert_st( blob_dims.dims(1) == (uint32_t)layer_blob->channels() );
      assert_st( blob_dims.dims(0) == (uint32_t)layer_blob->num() );
      assert_st( blob->elems.sz == uint32_t(layer_blob->count()) );

      const float * const data_ptr = &blob->elems[0];
      switch (Caffe::mode()) {
      case Caffe::CPU: memcpy( layer_blob->mutable_cpu_data(), data_ptr, sizeof(float) * layer_blob->count()); break;
      case Caffe::GPU: cudaMemcpy( layer_blob->mutable_gpu_data(), data_ptr,
				   sizeof(float) * layer_blob->count(), cudaMemcpyHostToDevice); break;
      default: rt_err( "Unknown Caffe mode." );
      }  // switch (Caffe::mode())
    }
  }
  void set_layer_blobs( p_Net_float net_, string const & layer_name, vect_p_nda_float_t & blobs ) {
    uint32_t const layer_ix = get_layer_ix( net_, layer_name );
    set_layer_blobs( net_, layer_ix, blobs );
  }


  // note: assumes/includes chans_to_area conversion
  u32_pt_t run_cnet_t::get_one_blob_img_out_sz( void ) {
    assert_st( net );
    uint32_t const out_layer_ix = get_layer_ix( net, out_layer_name );
    const vector<Blob<float>*>& output_blobs = net->top_vecs()[ out_layer_ix ];
    assert_st( output_blobs.size() == 1 );
    Blob<float> const * const output_blob = output_blobs[0];
    return u32_pt_t( output_blob->width(), output_blob->height() ).scale( u32_ceil_sqrt( output_blob->channels() ) );
  }


  p_nda_float_t run_one_blob_in_one_blob_out( p_Net_float net, string const & out_layer_name, p_nda_float_t const & in ) {
    assert_st( net );
    vect_p_nda_float_t in_data; 
    in_data.push_back( in ); // assume single input blob
    raw_do_forward( net, in_data );
    vect_p_nda_float_t out_data; 
    copy_output_blob_data( net, out_layer_name, out_data );
    assert( out_data.size() == 1 ); // assume single output blob
    return out_data.front();
  }
  p_nda_float_t run_cnet_t::run_one_blob_in_one_blob_out( void ) { 
    return boda::run_one_blob_in_one_blob_out( net, out_layer_name, in_batch ); }
  p_nda_float_t run_cnet_t::run_one_blob_in_one_blob_out_upsamp( void ) { 
    return boda::run_one_blob_in_one_blob_out( upsamp_net, out_layer_name, in_batch ); }

  // note; there is an unfortunate potential circular dependency here: we may need the pipe info
  // about the network before we have set it up if the desired size of the input image depends on
  // the net architeture (i.e. support size / padding / etc ).  currently, the only thing we need
  // the pipe for before setup is the number of images. this is determined by the blf_pack code
  // which needs the supports sizes and padding info from the pipe. but, since the pipe doesn't care
  // about the the number of input images (note: it does currently use in_sz to create the conv_ios
  // here, but that could be delayed), we can get away with creating the net_param first, then the
  // pipe, then altering num_input_images input_dim field of the net_param, then setting up the
  // net. hmm.
  p_conv_pipe_t run_cnet_t::cache_pipe( net_param_t & net_param ) {return create_pipe_from_param( net_param, in_num_chans, out_layer_name ); }

  struct synset_elem_t {
    string id;
    string tag;
  };
  std::ostream & operator <<(std::ostream & os, synset_elem_t const & v) { return os << "id=" << v.id << " tag=" << v.tag; }
  

  void read_synset( p_vect_synset_elem_t out, filename_t const & fn ) {
    p_ifstream ifs = ifs_open( fn );  
    string line;
    while( !ifs_getline( fn.in, ifs, line ) ) {
      size_t const spos = line.find( ' ' );
      if( spos == string::npos ) { rt_err( "failing to parse synset line: no space found: line was '" + line + "'" ); }
      size_t cpos = line.find( ',', spos+1 );
      if( cpos == string::npos ) { cpos = line.size(); }
      assert( spos < cpos );
      uint32_t const tag_len = cpos - spos - 1;
      if( !tag_len ) { rt_err( "failing to parse synset line: no tag found, (comma after first space) or (first space at end of line (note: implies no command after first space)): line was '" + line + "'" ); }
      synset_elem_t t;
      t.id = string( line, 0, spos );
      t.tag = string( line, spos+1, tag_len );
      out->push_back( t );
    }
    //printf( "out=%s\n", str(out).c_str() );
#if 0
    p_ofstream tags = ofs_open("tags.txt");
    for( vect_synset_elem_t::const_iterator i = out->begin(); i != out->end(); ++i ) {
      (*tags) << ( i->tag + "\n" );
    }
#endif
  }


  void run_cnet_t::main( nesi_init_arg_t * nia ) { 
    setup_cnet();
  }

  void run_cnet_t::create_net_param( void ) {
    // read the 'stock' deploy prototxt, and then override
    // the input dims using knowledge of the protobuf format.
    net_param = parse_and_upgrade_net_param_from_text_file( ptt_fn );
    assert_st( net_param->input_dim_size() == 4 );
    net_param->set_input_dim(0,in_num_imgs);
    net_param->set_input_dim(1,in_num_chans);
    net_param->set_input_dim(2,in_sz.d[1]);
    net_param->set_input_dim(3,in_sz.d[0]);
    if( enable_upsamp_net ) {
      upsamp_net_param.reset( new net_param_t( *net_param ) ); // start with copy of net_param
      // halve the stride and kernel size for the first layer and rename it to avoid caffe trying to load weights for it
      assert_st( upsamp_net_param->layer_size() ); // better have at least one layer
      caffe::LayerParameter * lp = upsamp_net_param->mutable_layer(0);
      if( !lp->has_convolution_param() ) { rt_err( "first layer of net not conv layer; don't know how to create upsampled network"); }
      caffe::ConvolutionParameter * cp = lp->mutable_convolution_param();
      p_conv_op_t conv_op = get_conv_op_from_param( *cp );
      // FIXME: we probably need to deal with padding better here?
      conv_op->kern_sz = ceil_div( conv_op->kern_sz, u32_pt_t{2,2} );
      assert_st( conv_op->in_pad.bnds_are_same() );
      conv_op->in_pad.p[0] = ceil_div( conv_op->in_pad.p[0], u32_pt_t{2,2} );
      conv_op->in_pad.p[1] = conv_op->in_pad.p[0];
      for( uint32_t i = 0; i != 2; ++i ) {
	if( (conv_op->stride.d[i]&1) ) { rt_err( "first conv layer has odd stride; don't know how to create upsampled network" ); }
	conv_op->stride.d[i] /= 2;
      }
      set_param_from_conv_op( *cp, conv_op );
      assert_st( lp->has_name() );
      lp->set_name( lp->name() + "-in-2X-us" );
    }
  }

  // most clients call this. others might inline it to be able to call setup_cnet_adjust_in_num_imgs()
  void run_cnet_t::setup_cnet( void ) {
    setup_cnet_param_and_pipe();
    // optionally, could call setup_cnet_adjust_in_num_imgs( ... ) here
    setup_cnet_net_and_batch();
  }

  conv_support_info_t const & run_cnet_t::get_out_csi( bool const & from_upsamp_net ) {
    p_conv_pipe_t from_pipe = from_upsamp_net ? conv_pipe_upsamp : conv_pipe;
    if( from_upsamp_net ) { assert_st( enable_upsamp_net && conv_pipe_upsamp ); }
    assert_st( from_pipe );
    return from_pipe->get_single_top_node()->csi;
  }
  conv_io_t const & run_cnet_t::get_out_cio( bool const & from_upsamp_net ) {
    p_conv_pipe_t from_pipe = from_upsamp_net ? conv_pipe_upsamp : conv_pipe;
    if( from_upsamp_net ) { assert_st( enable_upsamp_net && conv_pipe_upsamp ); }
    assert_st( from_pipe );
    return from_pipe->get_single_top_node()->cio;
  }

  void run_cnet_t::setup_cnet_param_and_pipe( void ) {
    assert( !net_param );
    create_net_param();
    conv_pipe = cache_pipe( *net_param );
    out_s = u32_ceil_sqrt( get_out_cio(0).chans );
    if( enable_upsamp_net ) { 
      conv_pipe_upsamp = cache_pipe( *upsamp_net_param );
      assert_st( out_s == u32_ceil_sqrt( get_out_cio(1).chans ) ); // FIXME: too strong?
    }
    conv_pipe->calc_sizes_forward( in_sz, 0 ); 
    if( enable_upsamp_net ) { conv_pipe_upsamp->calc_sizes_forward( in_sz, 0 ); }
  }
  void run_cnet_t::setup_cnet_adjust_in_num_imgs( uint32_t const in_num_imgs_ ) {
    assert_st( net_param && conv_pipe );
    assert_st( net_param->input_dim_size() == 4 );
    in_num_imgs = in_num_imgs_;
    net_param->set_input_dim(0,in_num_imgs);
    if( enable_upsamp_net ) { 
      upsamp_net_param->set_input_dim(0,in_num_imgs); } // FIXME/TODO: for now, run upsamp on all planes
  }

  void run_cnet_t::create_upsamp_layer_0_weights( void ) {
    vect_p_nda_float_t usl_blobs;
    copy_layer_blobs( net, 0, usl_blobs );

    vect_p_nda_float_t usl_blobs_upsamp;
    copy_layer_blobs( upsamp_net, 0, usl_blobs_upsamp );

    assert_st( usl_blobs.size() == 2 ); // filters, biases
    assert_st( usl_blobs_upsamp.size() == 2 ); // filters, biases
    assert_st( usl_blobs[1]->dims == usl_blobs_upsamp[1]->dims ); // biases should be same shape (and same strides?)
    usl_blobs_upsamp[1] = usl_blobs[1]; // use biases unchanged in upsamp net
    assert_st( usl_blobs[0]->dims.dims(0) == usl_blobs_upsamp[0]->dims.dims(0) );
    assert_st( usl_blobs[0]->dims.dims(1) == usl_blobs_upsamp[0]->dims.dims(1) );
    assert_st( u32_ceil_div( usl_blobs[0]->dims.dims(2), 2 ) == usl_blobs_upsamp[0]->dims.dims(2) );
    assert_st( u32_ceil_div( usl_blobs[0]->dims.dims(3), 2 ) == usl_blobs_upsamp[0]->dims.dims(3) );

    for( dims_iter_t di( usl_blobs_upsamp[0]->dims ) ; ; ) { usl_blobs_upsamp[0]->at(di.di) = 0; 
      if( !di.next() ) { break; } 
    }

    for( dims_iter_t di( usl_blobs[0]->dims ) ; ; ) { 
      usl_blobs_upsamp[0]->at4(di.di[0],di.di[1],di.di[2]>>1,di.di[3]>>1) += usl_blobs[0]->at( di.di );
      if( !di.next() ) { break; } 
    }

    set_layer_blobs( upsamp_net, 0, usl_blobs_upsamp );
  }

  void run_cnet_t::setup_cnet_net_and_batch( void ) {
    assert_st( !net );
    init_caffe( gpu_id ); // FIXME/note: only does something on first call
    net = caffe_create_net( *net_param, trained_fn.exp );      
    if( enable_upsamp_net ) { 
      upsamp_net = caffe_create_net( *upsamp_net_param, trained_fn.exp ); 
      create_upsamp_layer_0_weights();
    }

    assert_st( !in_batch );
    dims_t in_batch_dims( 4 );
    in_batch_dims.dims(3) = in_sz.d[0];
    in_batch_dims.dims(2) = in_sz.d[1];
    in_batch_dims.dims(1) = in_num_chans; 
    in_batch_dims.dims(0) = in_num_imgs;
    in_batch.reset( new nda_float_t( in_batch_dims ) );
  }

  void cnet_predict_t::main( nesi_init_arg_t * nia ) { 
    setup_cnet();
    setup_predict();
    p_img_t img_in( new img_t );
    img_in->load_fn( img_in_fn.exp );
    do_predict( img_in, 1 );
  }

  void cnet_predict_t::setup_predict( void ) {
    assert_st( !out_labels );
    out_labels.reset( new vect_synset_elem_t );
    read_synset( out_labels, out_labels_fn );

    assert( pred_state.empty() );
    if( scale_infos.empty() ) { setup_scale_infos(); } // if not specified, assume whole image / single scale 

    uint32_t const out_chans = get_out_cio(0).chans;

    if( get_out_csi(0).support_sz.is_zeros() ) { // only sensible in single-scale case 
      assert_st( scale_infos.size() == 1 );
      assert_st( scale_infos.back().img_sz == nominal_in_sz );
      assert_st( scale_infos.back().place.is_zeros() );
      assert_st( scale_infos.back().bix == 0 );
      //assert_st( enable_upsamp_net == 0 ); // too strong?
    }
    p_ofstream rps;
    if( dump_rps ) { rps = ofs_open("rps.txt"); }
    for( vect_scale_info_t::iterator i = scale_infos.begin(); i != scale_infos.end(); ++i ) {
      i->psb = pred_state.size();
      for( uint32_t bc = 0; bc < out_chans; ++bc ) {
	for( int32_t by = i->feat_box.p[0].d[1]; by < i->feat_box.p[1].d[1]; ++by ) {
	  for( int32_t bx = i->feat_box.p[0].d[0]; bx < i->feat_box.p[1].d[0]; ++bx ) {
	    pred_state.push_back( pred_state_t{} );
	    pred_state_t & ps = pred_state.back();
	    ps.label_ix = bc; 
	    u32_pt_t const feat_xy = {bx, by};
	    u32_box_t feat_pel_box{feat_xy,feat_xy+u32_pt_t{1,1}};
	    i32_box_t valid_in_xy, core_valid_in_xy; // note: core_valid_in_xy unused
	    unchecked_out_box_to_in_boxes( valid_in_xy, core_valid_in_xy, u32_to_i32( feat_pel_box ), 
					   get_out_csi(i->from_upsamp_net), i->img_sz );
	    valid_in_xy -= u32_to_i32(i->place); // shift so image nc is at 0,0
	    valid_in_xy = valid_in_xy * u32_to_i32(nominal_in_sz) / u32_to_i32(i->img_sz); // scale for scale
	    ps.img_box = valid_in_xy;
	    if( rps && (bc==0) ) { (*rps) << ps.img_box.parts_str() << "\n"; }
	  }
	}
      }
    }
  
  }

  // single scale case
  void cnet_predict_t::setup_scale_infos( void ) {
    u32_pt_t const & feat_sz = get_out_cio(0).sz;
    i32_box_t const valid_feat_box{{},u32_to_i32(feat_sz)};
    assert_st( valid_feat_box.is_strictly_normalized() );
    i32_box_t const valid_feat_img_box = valid_feat_box.scale(out_s);
    nominal_in_sz = in_sz;
    scale_infos.push_back( scale_info_t{nominal_in_sz,0,0,{},valid_feat_box,valid_feat_img_box} );
    assert_st( !enable_upsamp_net ); // unhandled(/nonsensical?)
  }

  void cnet_predict_t::setup_scale_infos( uint32_t const & interval, vect_u32_pt_t const & sizes, 
					  vect_u32_pt_w_t const & placements,
					  u32_pt_t const & nominal_in_sz_ ) {
    nominal_in_sz = nominal_in_sz_;
    conv_pipe->dump_pipe( std::cout );
    if( get_out_csi(0).support_sz.is_zeros() ) {
      rt_err( "global pooling and/or\n inner product layers + trying to "
	      "compute dense features = madness!" );
    } 
    assert( scale_infos.empty() );

    if( enable_upsamp_net ) {
      // should be at least one octave for using upsampling net to make sense
      assert_st( sizes.size() >= interval ); 
      scale_infos.resize( interval ); // preallocate space for the upsampled octave sizes
    }

    for( uint32_t six = 0; six < sizes.size(); ++six ) {
      uint32_t const bix = placements.at(six).w;
      u32_pt_t const dest = placements.at(six);
      u32_pt_t const sz = sizes.at(six);

      u32_box_t per_scale_img_box{dest,dest+sz};
      // assume we've ensured that there is eff_tot_pad around the scale_img
      per_scale_img_box.p[0] -= get_out_csi(0).eff_tot_pad.p[0];
      per_scale_img_box.p[1] += get_out_csi(0).eff_tot_pad.p[1];
      i32_box_t valid_feat_box;
      in_box_to_out_box( valid_feat_box, per_scale_img_box, cm_valid, get_out_csi(0) );
      assert_st( valid_feat_box.is_strictly_normalized() );      
      i32_box_t valid_feat_img_box = valid_feat_box.scale(out_s);
      scale_infos.push_back( scale_info_t{sz,0,bix,dest,valid_feat_box,valid_feat_img_box} ); // note: from_upsamp_net=0

      // if we're in the first placed octave, and the upsampling net
      // is enabled, add scale_infos for the in-net-upsampled octave
      // here.
      if( enable_upsamp_net && (six < interval) ) { 
	per_scale_img_box = u32_box_t{dest,dest+sz};
	// assume we've ensured that there is eff_tot_pad around the scale_img
	per_scale_img_box.p[0] -= get_out_csi(1).eff_tot_pad.p[0];
	per_scale_img_box.p[1] += get_out_csi(1).eff_tot_pad.p[1];

	in_box_to_out_box( valid_feat_box, per_scale_img_box, cm_valid, get_out_csi(1) );
	assert_st( valid_feat_box.is_strictly_normalized() );
	valid_feat_img_box = valid_feat_box.scale(out_s); // FIXME: sort-of-not-right (wrong net out_s)
	scale_infos[six] = scale_info_t{sz,1,bix,dest,valid_feat_box,valid_feat_img_box}; // note: from_upsamp_net=1
      }
    }
    

    printf( "scale_infos=%s\n", str(scale_infos).c_str() );
  }

  template< typename T > struct gt_filt_prob {
    vector< T > const & v;
    gt_filt_prob( vector< T > const & v_ ) : v(v_) {}
    bool operator()( uint32_t const & ix1, uint32_t const & ix2 ) { 
      assert_st( ix1 < v.size() );
      assert_st( ix2 < v.size() );
      return v[ix1].filt_prob > v[ix2].filt_prob;
    }
  };

  typedef map< i32_box_t, anno_t > anno_map_t;

  // example command line for testing/debugging detection code:
  // boda capture_classify --cnet-predict='(in_sz=600 600,ptt_fn=%(models_dir)/nin_imagenet_nopad/deploy.prototxt,trained_fn=%(models_dir)/nin_imagenet_nopad/best.caffemodel,out_layer_name=relu12)' --capture='(cap_res=640 480)'

  //p_vect_anno_t cnet_predict_t::do_predict( p_img_t const & img_in, bool const print_to_terminal ) { }

  p_vect_anno_t cnet_predict_t::do_predict( p_img_t const & img_in, bool const print_to_terminal ) {
    p_img_t img_in_ds = resample_to_size( img_in, in_sz );
    subtract_mean_and_copy_img_to_batch( in_batch, 0, img_in_ds );
    p_nda_float_t out_batch = run_one_blob_in_one_blob_out();
    p_nda_float_t out_batch_upsamp;
    if( enable_upsamp_net ) { out_batch_upsamp = run_one_blob_in_one_blob_out_upsamp(); }
    return do_predict( out_batch, out_batch_upsamp, print_to_terminal );
  }

  p_vect_anno_t cnet_predict_t::do_predict( p_nda_float_t const & out_batch, p_nda_float_t const & out_batch_upsamp, 
					    bool const print_to_terminal ) {

    for( vect_scale_info_t::iterator i = scale_infos.begin(); i != scale_infos.end(); ++i ) {
      p_nda_float_t scale_batch = i->from_upsamp_net ? out_batch_upsamp : out_batch;
      dims_t const & sbd = scale_batch->dims;
      assert( sbd.sz() == 4 );
      assert( sbd.dims(1) == out_labels->size() );

      dims_t img_e( scale_batch->dims.sz() );
      dims_t img_b( img_e.sz() );
      img_b.dims(0) = i->bix;
      img_e.dims(0) = i->bix + 1;
      img_b.dims(1) = 0;
      img_e.dims(1) = sbd.dims(1);
      img_b.dims(2) = i->feat_box.p[0].d[1];
      img_e.dims(2) = i->feat_box.p[1].d[1];
      img_b.dims(3) = i->feat_box.p[0].d[0];
      img_e.dims(3) = i->feat_box.p[1].d[0];
      assert_st( img_e.fits_in( sbd ) );
      assert_st( img_b.fits_in( img_e ) );
      do_predict_region( scale_batch, img_b, img_e, i->psb );
    }
    return pred_state_to_annos( print_to_terminal );
  }


  i32_box_t cnet_predict_t::nms_grid_op( bool const & do_set, i32_box_t const & img_box ) {
    uint32_t tot_pel = 0;
    uint32_t over_pel = 0;

    i32_box_t shrunk_quant_img_box = floor_div( img_box.scale_and_round( nms_core_rat ), nms_grid_pels );

    nms_grid_t::iterator ci = nms_grid.find( shrunk_quant_img_box.center_rd() );
    i32_box_t center_match;
    if( ci != nms_grid.end() ) { center_match = ci->second; }
    uint32_t center_match_cnt = 0;

    for( int32_t by = shrunk_quant_img_box.p[0].d[1]; by < shrunk_quant_img_box.p[1].d[1]; ++by ) {
      for( int32_t bx = shrunk_quant_img_box.p[0].d[0]; bx < shrunk_quant_img_box.p[1].d[0]; ++bx ) {
	i32_pt_t const pel{bx,by};
	if( do_set ) { nms_grid[pel] = img_box; }
	else {
	  ++tot_pel;
	  nms_grid_t::iterator i = nms_grid.find( pel );
	  if( i != nms_grid.end() ) { ++over_pel; if( i->second == center_match ) { ++center_match_cnt; } }
	}
      }
    }
    if( center_match_cnt * 4 > tot_pel * 3 ) {  // mostly covers an existing match, so maybe add anno to that match
      assert_st( over_pel );
      return center_match;
    } else if( over_pel ) {
      return i32_box_t{};
    } else { return img_box; } // doesn't overlap
  }

  p_vect_anno_t cnet_predict_t::pred_state_to_annos( bool const print_to_terminal ) {
    anno_map_t annos;
    if( print_to_terminal ) {
      printf("\033[2J\033[1;1H");
      printf("---- frame -----\n");
    }
    vect_uint32_t disp_list;
    for( uint32_t i = 0; i < pred_state.size(); ++i ) {  if( pred_state[i].to_disp ) { disp_list.push_back(i); } }
    sort( disp_list.begin(), disp_list.end(), gt_filt_prob<pred_state_t>( pred_state ) );
    uint32_t num_disp = 0;
    nms_grid.clear();
    for( vect_uint32_t::const_iterator ii = disp_list.begin(); ii != disp_list.end(); ++ii ) {
      if( num_disp == max_num_disp ) { break; }
      pred_state_t const & ps = pred_state[*ii];
      // check nms
      i32_box_t const nms_box = nms_grid_op( 0, ps.img_box );
      if( nms_box == i32_box_t{} ) { continue; } //nms suppression condition: overlaps other core and no close-center-match
      anno_map_t::iterator ami = annos.find( nms_box ); // either ps.img_box or a close-matching overlap
      // nms supression condition: existing-anno-full 
      if( (ami != annos.end()) && (ami->second.item_cnt >= max_labels_per_anno) ) { continue; } 
      
      anno_t & anno = annos[nms_box];
      if( ami == annos.end() ) { // was new, init
	assert( nms_box == ps.img_box );
	anno.item_cnt = 0;
	nms_grid_op( 1, ps.img_box );
      } 
      bool const did_ins = anno.seen_label_ixs.insert( ps.label_ix ).second;
      if( !did_ins ) { continue; } // ignore dup labels 
      string anno_str;
      if( anno_mode == 0 ) {
	anno_str = strprintf( "%-20s -- filt_p=%-10s p=%-10s\n", str(out_labels->at(ps.label_ix).tag).c_str(), 
			      str(ps.filt_prob).c_str(),
			      str(ps.cur_prob).c_str() );
      } else if ( anno_mode == 1 ) {
	anno_str = strprintf( "%s\n", str(out_labels->at(ps.label_ix).tag).c_str() ); 
      } else if ( anno_mode == 3 ) {
	anno_str = strprintf( "%-4s %s\n", str(ii - disp_list.begin() + 1).c_str(), str(out_labels->at(ps.label_ix).tag).c_str() ); 
      } else if ( anno_mode == 2 ) {
	anno_str = strprintf( "%-20s -- sz=%s\n", 
			      str(out_labels->at(ps.label_ix).tag).c_str(), str(ps.img_box.sz()).c_str() ); 
      }

      anno.str += anno_str;
      ++anno.item_cnt;
      if( print_to_terminal ) { printstr( anno_str ); }
      ++num_disp;
    }
    if( print_to_terminal ) { printf("---- end frame -----\n"); }
    //printf( "obd=%s\n", str(obd).c_str() );
    //(*ofs_open( out_fn )) << out_pt_str;
    p_vect_anno_t ret_annos( new vect_anno_t );
    for( anno_map_t::const_iterator i = annos.begin(); i != annos.end(); ++i ) { 
      ret_annos->push_back( i->second ); 
      anno_t & anno = ret_annos->back();
      anno.box = i->first; 
      anno.fill = 0; anno.box_color = rgba_to_pel(170,40,40); anno.str_color = rgba_to_pel(220,220,255);
    }
    return ret_annos;
  }

  // fills in (part of) pred_state
  void cnet_predict_t::do_predict_region( p_nda_float_t const & out_batch, dims_t const & obb, dims_t const & obe,
					  uint32_t const & psb ) {
    dims_t const ob_span = obe - obb;
    assert( ob_span.sz() == 4 );
    assert( ob_span.dims(0) == 1 );
    assert( ob_span.dims(1) == out_labels->size() );

    uint32_t const num_pred = ob_span.dims_prod();
    assert_st( (psb+num_pred) <= pred_state.size() );
    uint32_t const num_pels = ob_span.dims(2)*ob_span.dims(3);
    vect_double pel_sums( num_pels, 0.0 );
    vect_double pel_maxs( num_pels, 0.0 );

    { uint32_t pel_ix = 0; uint32_t psix = psb; for( dims_iter_t di( obb, obe ) ; ; ++psix, ++pel_ix ) { 
	if( pel_ix == num_pels ) { pel_ix = 0; }
	double p = out_batch->at(di.di);
	pred_state.at(psix).cur_prob = p;
	max_eq( pel_maxs[pel_ix], p ) ;
	pel_sums[pel_ix] += p;
	if( !di.next() ) { break; } 
      }
    }    

    vect_uint32_t pel_is_pdf( num_pels, 0 );
    uint32_t tot_num_pdf = 0;
    // if the pel looks ~like a PDF, we leave it as is. otherwise, we apply a softmax
    for( uint32_t i = 0; i != num_pels; ++i ) {
      if( (fabs( pel_sums[i] - 1.0 ) < .01) && (pel_maxs[i] < 1.01)  ) { pel_is_pdf[i] = 1; ++tot_num_pdf; }
      pel_sums[i] = 0; // reused below if pel_is_pdf is false
    }
    //printf( "num_pels=%s tot_num_pdf=%s\n", str(num_pels).c_str(), str(tot_num_pdf).c_str() );

    // FIXME: is it wrong/bad for is_pdf to ber per-pel? should it be all-or-none somehow?
    { uint32_t pel_ix = 0; for( uint32_t psix = psb; psix != psb+num_pred; ++psix, ++pel_ix ) { 
	if( pel_ix == num_pels ) { pel_ix = 0; }
	if( !pel_is_pdf.at(pel_ix) ) { // if not already a PDF, apply a softmax
	  double & p = pred_state[psix].cur_prob;
	  double exp_p = exp(p - pel_maxs[pel_ix]);
	  p = exp_p;
	  pel_sums[pel_ix] += exp_p;
	}
      }
    }
    //printf( "pel_sums=%s pel_maxs=%s\n", str(pel_sums).c_str(), str(pel_maxs).c_str() );
    { uint32_t pel_ix = 0; for( uint32_t psix = psb; psix != psb+num_pred; ++psix, ++pel_ix ) { 
	if( pel_ix == num_pels ) { pel_ix = 0; }
	if( !pel_is_pdf.at(pel_ix) ) { pred_state.at(psix).cur_prob /= pel_sums.at(pel_ix); } // rest of softmax
      }
    }    

    // temportal filtering and setting to_disp
    { uint32_t pel_ix = 0; for( uint32_t psix = psb; psix != psb+num_pred; ++psix, ++pel_ix ) { 
	if( pel_ix == num_pels ) { pel_ix = 0; }
	pred_state_t & ps = pred_state.at(psix);

	if( !ps.filt_prob_init ) { ps.filt_prob_init = 1; ps.filt_prob = ps.cur_prob; }
	else { ps.filt_prob *= (1 - filt_rate); ps.filt_prob += ps.cur_prob * filt_rate; }

	if( ps.filt_prob >= filt_show_thresh ) { ps.to_disp = 1; }
	else if( ps.filt_prob <= filt_drop_thresh ) { ps.to_disp = 0; }
      }
    }
  }

  struct cnet_mod_t : virtual public nesi // NESI(help="base class for utilities to modify caffe nets" )
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support
    filename_t ptt_fn; //NESI(default="%(models_dir)/%(in_model)/train_val.prototxt",help="input net prototxt template filename")
    filename_t trained_fn; //NESI(default="%(models_dir)/%(in_model)/best.caffemodel",help="input trained net from which to copy params")
    filename_t mod_fn; //NESI(default="%(models_dir)/%(out_model)/train_val.prototxt",help="output net prototxt template filename")
    filename_t mod_weights_fn; //NESI(default="%(models_dir)/%(out_model)/boda_gen.caffemodel",help="output net weights binary prototxt template filename")

    p_net_param_t net_param;
    p_net_param_t mod_net_param;

    void ensure_out_dir( nesi_init_arg_t * const nia ) { ensure_is_dir( nesi_filename_t_expand( nia, "%(models_dir)/%(out_model)" ), 1 ); }
    void create_net_params( void ) {
      net_param = parse_and_upgrade_net_param_from_text_file( ptt_fn );
      mod_net_param.reset( new net_param_t( *net_param ) ); // start with copy of net_param
    }
    void write_mod_pt( void ) {
      string mod_str;
      bool const pts_ret = google::protobuf::TextFormat::PrintToString( *mod_net_param, &mod_str );
      assert_st( pts_ret );
      write_whole_fn( mod_fn, mod_str );
    }
    p_Net_float net;
    p_Net_float mod_net;
    void load_nets( void ) {
      net = caffe_create_net( *net_param, trained_fn.exp );      
      mod_net = caffe_create_net( *mod_net_param, trained_fn.exp ); 
    }
    void write_mod_net( void ) {
      p_net_param_t mod_net_param_with_weights;
      mod_net_param_with_weights.reset( new net_param_t );
      mod_net->ToProto( mod_net_param_with_weights.get(), false );
      caffe::WriteProtoToBinaryFile( *mod_net_param_with_weights, mod_weights_fn.exp );
    }
  };

  struct cnet_fc_to_conv_t : virtual public nesi, public cnet_mod_t, public has_main_t // NESI(help="utility to modify caffe nets",
			     // bases=["cnet_mod_t","has_main_t"], type_id="cnet_fc_to_conv")
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support

    void main( nesi_init_arg_t * nia ) { 
      init_caffe( 0 );
      create_net_params();
      net = caffe_create_net( *net_param, trained_fn.exp ); // we need to load the original weights 'early' to infer input dims

      vect_string converted_layer_names;
      uint32_t const numl = (uint32_t)mod_net_param->layer_size();
      // find and rename all fc layers
      for( uint32_t i = 0; i != numl; ++i ) {
	caffe::LayerParameter * lp = mod_net_param->mutable_layer(i);
	if( lp->type() != InnerProduct_str ) { continue; }
	vect_p_nda_float_t blobs;
	copy_layer_blobs( net, lp->name(), blobs );

	caffe::InnerProductParameter * ipp = lp->mutable_inner_product_param();
	converted_layer_names.push_back( lp->name() );
	lp->set_name( lp->name() + "-conv" );
	lp->set_type( Convolution_str );

	caffe::ConvolutionParameter * cp = lp->mutable_convolution_param();
	assert_st( ipp->has_num_output() );
	if( ipp->has_num_output() ) { cp->set_num_output( ipp->num_output() ); }
	if( ipp->has_bias_term() ) { cp->set_bias_term( ipp->bias_term() ); }
	if( ipp->has_weight_filler() ) { *cp->mutable_weight_filler() = ipp->weight_filler(); }
	if( ipp->has_bias_filler() ) { *cp->mutable_bias_filler() = ipp->bias_filler(); }

	assert_st( blobs.size() == 2 ); // filters, biases
	//printf( "lp->name()=%s\n", str(lp->name()).c_str() );
	//printf( "net_param->mutable_layer(i)->name()=%s\n", str(net_param->mutable_layer(i)->name()).c_str() );
	//printf( "blobs[0]->dims=%s\n", str(blobs[0]->dims).c_str() );
	assert_st( blobs[0]->dims.dims(0) == 1 );
	assert_st( blobs[0]->dims.dims(1) == 1 );
	assert_st( blobs[0]->dims.dims(2) == ipp->num_output() );
	uint32_t num_w = blobs[0]->dims.dims(3);

	// get number of input chans
	if( lp->bottom_size() != 1) { rt_err( "unhandled: bottom_size() != 1"); }
	string const bot_bn = lp->bottom(0);
	assert_st( net->has_blob( bot_bn ) );
	uint32_t const num_in_chan = net->blob_by_name( bot_bn )->channels();

	// FIXME: we assume input is spactially square, which may not be true
	assert_st( !(num_w % num_in_chan) );
	uint32_t kern_sz = sqrt(num_w / num_in_chan);
	assert_st( kern_sz*kern_sz*num_in_chan == num_w );
	cp->set_kernel_size( kern_sz );
	lp->clear_inner_product_param();
      }
      ensure_out_dir( nia );
      write_mod_pt();
      mod_net = caffe_create_net( *mod_net_param, trained_fn.exp );
      for( vect_string::const_iterator i = converted_layer_names.begin(); i != converted_layer_names.end(); ++i ) {
	fc_weights_to_conv_weights( *i );
      }
      write_mod_net();
    }

    void fc_weights_to_conv_weights( string const & layer_name ) {
      vect_p_nda_float_t blobs;
      copy_layer_blobs( net, layer_name, blobs );

      vect_p_nda_float_t blobs_mod;
      copy_layer_blobs( mod_net, layer_name + "-conv", blobs_mod );

      assert_st( blobs.size() == 2 ); // filters, biases
      assert_st( blobs_mod.size() == 2 ); // filters, biases
      assert_st( blobs[1]->dims == blobs_mod[1]->dims ); // biases should be same shape (and same strides?)
      blobs_mod[1] = blobs[1]; // use biases unchanged in upsamp net

      assert( blobs_mod[0]->dims.dims_prod() == blobs[0]->dims.dims_prod() );
      assert( blobs_mod[0]->elems.sz == blobs[0]->elems.sz );
      blobs_mod[0]->elems = blobs[0]->elems; // reshape

      set_layer_blobs( mod_net, layer_name + "-conv", blobs_mod );
    }

  };

  void resize_kernel( p_nda_float_t const & in, p_nda_float_t const & out );

  struct cnet_resize_conv_t : virtual public nesi, public cnet_mod_t, public has_main_t // NESI(help="utility to modify caffe nets",
		       // bases=["cnet_mod_t","has_main_t"], type_id="cnet_resize_conv")
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support
    string to_resize_ln;//NESI(default="conv1",help="name of conv layer to resize ")    
    u32_pt_t targ_sz; //NESI(default="5 5",help="kernel size for resized layer")

    void resize_conv_weights( string const & to_resize_name ) {
      vect_p_nda_float_t blobs;
      copy_layer_blobs( net, to_resize_name, blobs );

      vect_p_nda_float_t blobs_mod;
      copy_layer_blobs( mod_net, to_resize_name + "-resized", blobs_mod );

      assert_st( blobs.size() == 2 ); // filters, biases
      assert_st( blobs_mod.size() == 2 ); // filters, biases
      assert_st( blobs[1]->dims == blobs_mod[1]->dims ); // biases should be same shape (and same strides?)
      blobs_mod[1] = blobs[1]; // use biases unchanged in upsamp net
      assert_st( blobs[0]->dims.dims(0) == blobs_mod[0]->dims.dims(0) );
      assert_st( blobs[0]->dims.dims(1) == blobs_mod[0]->dims.dims(1) );
      assert_st( targ_sz.d[1] == blobs_mod[0]->dims.dims(2) );
      assert_st( targ_sz.d[0] == blobs_mod[0]->dims.dims(3) );
      resize_kernel( blobs[0], blobs_mod[0] );
      set_layer_blobs( mod_net, to_resize_name + "-resized", blobs_mod );
    }

    void main( nesi_init_arg_t * nia ) { 
      init_caffe(0);
      create_net_params();

#if 0
      vect_float in1 = {0,1,5};
      vect_float out1 = {3,4,5,6,7};
      resize_1d( &in1[0], in1.size(), &out1[0], out1.size() );
      printf( "in1=%s out1=%s\n", str(in1).c_str(), str(out1).c_str() );
      return;
#endif

      uint32_t const to_resize_ix = get_layer_ix( *net_param, to_resize_ln );
      caffe::LayerParameter * lp = mod_net_param->mutable_layer(to_resize_ix);
      if( !lp->has_convolution_param() ) { 
	rt_err( strprintf("layer %s of net not conv layer; don't know how to resize",to_resize_ln.c_str())); }
      caffe::ConvolutionParameter * cp = lp->mutable_convolution_param();
      p_conv_op_t conv_op = get_conv_op_from_param( *cp );
      conv_op->kern_sz = targ_sz;

      set_param_from_conv_op( *cp, conv_op );
      assert_st( lp->has_name() );
      lp->set_name( lp->name() + "-resized" );

      uint32_t const numl = (uint32_t)mod_net_param->layer_size();
      // find and rename all fc layers
      for( uint32_t i = to_resize_ix + 1; i != numl; ++i ) {
	caffe::LayerParameter * lp = mod_net_param->mutable_layer(i);
	if( lp->type() == InnerProduct_str ) {
	  // FIXME: convert to conv layer. for now, just rename.
	  printf("WARNING: renaming fc/InnerProduct %s layer to avoid size mismatch when loading weights. note that the renamed layer in the output model will *not* get any copied weights from the input model!\n",lp->name().c_str()); 
	  lp->set_name( lp->name() + "-renamed-due-to-resize" );
	} 
      }
      ensure_out_dir( nia );
      write_mod_pt();
      load_nets();
      resize_conv_weights( to_resize_ln );
      write_mod_net();
    }
  };

#include"gen/caffeif.H.nesi_gen.cc"
#include"gen/caffeif.cc.nesi_gen.cc"
}

