// Copyright (c) 2013-2014, Matthew W. Moskewicz <moskewcz@alumni.princeton.edu>; part of Boda framework; see LICENSE
#include"boda_tu_base.H"
#include"timers.H"
#include"str_util.H"
#include"img_io.H"
#include"lexp.H"

#include "caffeif.H"
#include <glog/logging.h>
#include <google/protobuf/text_format.h>

namespace boda 
{
  using namespace boost;
  using caffe::Caffe;
  using caffe::Blob;
  
  void subtract_mean_and_copy_img_to_batch( p_nda_float_t const & in_batch, uint32_t img_ix, p_img_t const & img ) {
    dims_t const & ibd = in_batch->dims;
    assert_st( img_ix < ibd.dims(0) );
    assert_st( 3 == ibd.dims(1) );
    assert_st( img->w == ibd.dims(3) );
    assert_st( img->h == ibd.dims(2) );
    uint32_t const inmc = 123U+(117U<<8)+(104U<<16)+(255U<<24); // RGBA
#pragma omp parallel for	  
    for( uint32_t y = 0; y < ibd.dims(2); ++y ) {
      for( uint32_t x = 0; x < ibd.dims(3); ++x ) {
	uint32_t const pel = img->get_pel(x,y);
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

  void copy_batch_to_img( p_nda_float_t const & out_batch, uint32_t img_ix, p_img_t const & img ) {
    
    dims_t const & obd = out_batch->dims;
    assert( obd.sz() == 4 );
    assert_st( img_ix < obd.dims(0) );

    u32_pt_t const out_sz( obd.dims(3), obd.dims(2) );
    uint32_t sqrt_out_chan;
    u32_pt_t img_sz;
    chans_to_area( sqrt_out_chan, img_sz, out_sz, obd.dims(1) );
    assert( sqrt_out_chan );
    assert( (sqrt_out_chan*sqrt_out_chan) >= obd.dims(1) );

    // set up dim iterators that span only the image we want to process
    dims_t img_e = out_batch->dims;
    dims_t img_b( img_e.sz() );
    img_b.dims(0) = img_ix;
    img_e.dims(0) = img_ix + 1;
    float const out_max = nda_reduce( *out_batch, max_functor<float>(), 0.0f, img_b, img_e ); // note clamp to 0
    //float const out_min = nda_reduce( *out_batch, min_functor<float>(), 0.0f, img_b, img_e ); // note clamp to 0
    //assert_st( out_min == 0.0f ); // shouldn't be any negative values
    //float const out_rng = out_max - out_min;

    assert_st( u32_pt_t(img->w,img->h) == img_sz );

    for( uint32_t y = 0; y < img->h; ++y ) {
      for( uint32_t x = 0; x < img->w; ++x ) {
	uint32_t const bx = x / sqrt_out_chan;
	uint32_t const by = y / sqrt_out_chan;
	uint32_t const bc = (y%sqrt_out_chan)*sqrt_out_chan + (x%sqrt_out_chan);
	uint32_t gv;
	if( bc < obd.dims(1) ) {
	  //float const norm_val = ((out_batch->at4(img_ix,bc,by,bx)-out_min) / out_rng );
	  float const norm_val = ((out_batch->at4(img_ix,bc,by,bx)) / out_max );
	  gv = grey_to_pel( uint8_t( std::min( 255.0, 255.0 * norm_val ) ) );
	  //gv = grey_to_pel( uint8_t( std::min( 255.0, 255.0 * (log(.01) - log(std::max(.01f,norm_val))) / (-log(.01)) )));
	} else { gv = grey_to_pel( 0 ); }
	img->set_pel( x, y, gv );
      }
    }
  }

  p_Net_float init_caffe( string const & param_str, string const & trained_fn ) {
    timer_t t("caffe_init");
    google::InitGoogleLogging("boda_caffe");
    Caffe::set_phase(Caffe::TEST);
    Caffe::set_mode(Caffe::GPU);
    Caffe::SetDevice(0);
    //Caffe::set_mode(Caffe::CPU);

    caffe::NetParameter param;
    bool const ret = google::protobuf::TextFormat::ParseFromString( param_str, &param );
    assert_st( ret );

    p_Net_float net( new Net_float( param ) );
    net->CopyTrainedLayersFrom( trained_fn );

#if 0
    int total_iter = 10; // atoi(argv[3]);
    LOG(ERROR) << "Running " << total_iter << " iterations.";

    double test_accuracy = 0;
    for (int i = 0; i < total_iter; ++i) {
      const vector<Blob<float>*>& result = net->ForwardPrefilled();
      test_accuracy += result[0]->cpu_data()[0];
      LOG(ERROR) << "Batch " << i << ", accuracy: " << result[0]->cpu_data()[0];
    }
    test_accuracy /= total_iter;
    LOG(ERROR) << "Test accuracy: " << test_accuracy;
#endif
    return net;
  }


  void raw_do_forward( p_Net_float net_, vect_p_nda_float_t const & bottom ) {
    timer_t t("caffe_forward");
    vector<caffe::Blob<float>*>& input_blobs = net_->input_blobs();
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
    rt_err( strprintf("layer out_layer_name=%s not found in netowrk\n",str(out_layer_name).c_str() )); 
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
      p_nda_float_t out_batch( new nda_float_t );
      out_batch->set_dims( out_batch_dims );
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
  }


  void run_cnet_t::main( nesi_init_arg_t * nia ) { 
    setup_cnet();
  }

  void run_cnet_t::setup_cnet( void ) {
    p_string ptt_str = read_whole_fn( ptt_fn );
    string out_pt_str;

    dims_t in_batch_dims( 4 );
    in_batch_dims.dims(3) = in_sz.d[0];
    in_batch_dims.dims(2) = in_sz.d[1];
    in_batch_dims.dims(1) = in_num_chans; 
    in_batch_dims.dims(0) = in_num_imgs;

    str_format_from_nvm_str( out_pt_str, *ptt_str, 
			     strprintf( "(xsize=%s,ysize=%s,num=%s,chan=%s)", 
					str(in_batch_dims.dims(3)).c_str(), str(in_batch_dims.dims(2)).c_str(), 
					str(in_batch_dims.dims(0)).c_str(), str(in_batch_dims.dims(1)).c_str() ) );
    assert_st( !net );
    net = init_caffe( out_pt_str, trained_fn.exp );      

    assert_st( !in_batch );
    in_batch.reset( new nda_float_t );
    in_batch->set_dims( in_batch_dims );
  }

  void cnet_predict_t::main( nesi_init_arg_t * nia ) { 
    setup_predict();
    p_img_t img_in( new img_t );
    img_in->load_fn( img_in_fn.exp );
    do_predict( img_in );
  }

  void cnet_predict_t::setup_predict( void ) {
    assert_st( !out_labels );
    out_labels.reset( new vect_synset_elem_t );
    read_synset( out_labels, out_labels_fn );
    setup_cnet();
  }

  template< typename T > struct gt_indexed {
    vector< T > const & v;
    gt_indexed( vector< T > const & v_ ) : v(v_) {}
    bool operator()( uint32_t const & ix1, uint32_t const & ix2 ) { 
      assert_st( ix1 < v.size() );
      assert_st( ix2 < v.size() );
      return v[ix1] > v[ix2];
    }
  };

  void cnet_predict_t::do_predict( p_img_t const & img_in ) {
    p_img_t img_in_ds = downsample_to_size( downsample_2x_to_size( img_in, in_sz.d[0], in_sz.d[1] ), 
					    in_sz.d[0], in_sz.d[1] );
    subtract_mean_and_copy_img_to_batch( in_batch, 0, img_in_ds );
    p_nda_float_t out_batch = run_one_blob_in_one_blob_out();

    dims_t const & obd = out_batch->dims;
    assert( obd.sz() == 4 );
    assert( obd.dims(0) == 1 );
    assert( obd.dims(1) == out_labels->size() );
    assert( obd.dims(2) == 1 );
    assert( obd.dims(3) == 1 );

    bool const init_filt_prob = filt_prob.empty();
    if( init_filt_prob ) { filt_prob.resize( obd.dims(1) ); to_disp.resize( obd.dims(1), 0 ); }
    for( uint32_t i = 0; i < obd.dims(1); ++i ) {
      float const p = out_batch->at4(0,i,0,0);
      if( init_filt_prob ) { filt_prob[i] = p; }
      else { filt_prob[i] *= (1 - filt_rate); filt_prob[i] += p * filt_rate; }
      if( filt_prob[i] >= filt_show_thresh ) { to_disp[i] = 1; }
      else if( filt_prob[i] <= filt_drop_thresh ) { to_disp[i] = 0; }
    }
    printf("\033[2J\033[1;1H");
    printf("---- frame -----\n");
    vect_uint32_t disp_list;
    for( uint32_t i = 0; i < to_disp.size(); ++i ) {  if( to_disp[i] ) { disp_list.push_back(i); } }
    sort( disp_list.begin(), disp_list.end(), gt_indexed<float>( filt_prob ) );
    for( vect_uint32_t::const_iterator ii = disp_list.begin(); ii != disp_list.end(); ++ii ) {
      uint32_t const i = *ii;
      float const p = out_batch->at4(0,i,0,0);
      printf( "%-20s -- filt_p=%-10s p=%-10s\n", str(out_labels->at(i).tag).c_str(), 
	      str(filt_prob[i]).c_str(),
	      str(p).c_str() );
    }
    printf("---- end frame -----\n");
    //printf( "obd=%s\n", str(obd).c_str() );
    //(*ofs_open( out_fn )) << out_pt_str;
  }

#include"gen/caffeif.H.nesi_gen.cc"
}

