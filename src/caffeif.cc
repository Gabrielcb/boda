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
#include<google/protobuf/text_format.h>
#include"anno_util.H"
#include"rand_util.H"
#include"imagenet_util.H"

#include"gen/caffe.pb.h" 

namespace boda 
{

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
#pragma omp parallel for	  
    for( uint32_t y = 0; y < ibd.dims(2); ++y ) {
      for( uint32_t x = 0; x < ibd.dims(3); ++x ) {
	uint32_t const pel = img->get_pel({x,y});
	for( uint32_t c = 0; c < 3; ++c ) {
	  // note: RGB -> BGR swap via the '2-c' below
	  in_batch->at4( img_ix, 2-c, y, x ) = get_chan(c,pel) - float(uint8_t(u32_rgba_inmc >> (c*8)));
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


  // note: assumes/includes chans_to_area conversion
  u32_pt_t run_cnet_t::get_one_blob_img_out_sz( void ) {
    p_conv_node_t out_node = conv_pipe->must_get_node( out_node_name );
    return out_node->cio.sz.scale( u32_ceil_sqrt( out_node->cio.chans ) );
  }


  p_nda_float_t run_cnet_t::run_one_blob_in_one_blob_out( void ) { return conv_pipe->run_one_blob_in_one_blob_out( in_batch, conv_fwd ); }
  p_nda_float_t run_cnet_t::run_one_blob_in_one_blob_out_upsamp( void ) { 
    return conv_pipe_upsamp->run_one_blob_in_one_blob_out( in_batch, conv_fwd_upsamp );
  }

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
    p_nda_float_t out_batch_1 = run_one_blob_in_one_blob_out();
  }

  void run_cnet_t::create_net_param( void ) {
    // read the 'stock' deploy prototxt, and then override the input dims using knowledge of the
    // protobuf format.  also, if there are no inputs / input dims, assume we're reading a train_val-style
    // prototxt and adjust it as needed for our usage -- in particular by dropping data layers and
    // adding input dims (i.e. converting it on-the-fly to deploy format).

    // also, if specified, delete any layers after (in declaration order) the one that produces the specified node. note that if
    // the layers aren't declared in a 'good' (i.e. topo-sort) order for this to be sensible, then you get what you get.
    bool found_out_node = out_node_name.empty(); // if no layer name input, don't try to find a 'stopping/end' layer

    net_param = parse_and_upgrade_net_param_from_text_file( ptt_fn );

    // we assume there is single input blob named 'data', unless we see (and remove) a Data layer,
    // then we use the first top blob name from the last such removed layer. FIXME: do better?
    string data_node_name = "data";
    string label_node_name = "label";
    string next_loss_node_name = "loss"; // name any un-unamed loss layers "loss", "loss_2", ... 
    uint32_t nlnn_ix = 1;
    int o = 0;
    for( int i = 0; i < net_param->layer_size(); i++ ) {
      caffe::LayerParameter const * const lp = &net_param->layer(i);
      if( lp->type() == Data_str ) {
	// assume first top is name of data layer image data output blob
	if( lp->top_size() != 2 ) { rt_err( "unhandled caffe data layer with num inputs != 2" ); }
	data_node_name = lp->top(0);
	label_node_name = lp->top(1);
	continue; // drop layer
      } else if( lp->type() == Accuracy_str ) {
	continue; // drop layer
      } else if( (lp->type() == SoftmaxWithLoss_str) || (lp->type() == Softmax_str) ) {
	if( !add_bck_ops ) { continue; } // drop layer unless we're doing bck 
      }
      // if we got here, we keep layer (but may modify it)
      caffe::LayerParameter * const olp = net_param->mutable_layer(o);
      if( i != o ) { *olp = net_param->layer(i); } ++o; // keep layer

      // we don't use caffe's layer filtering, so strip any filtering info out (i.e. use this layer in 'all' phases).
      // FIXME/HACK/NOTE: this is not in general correct/sound. but generally the only layer with phase info that we keep is
      // SoftmaxWithLoss_str, and we only keep it when we want to run it (when add_bck_ops==1). we could probably at least error
      // check this better.
      olp->clear_phase(); olp->clear_include(); olp->clear_exclude(); 

      // keep SoftmaxWithLoss only when add_bck_ops==1; add a named loss output if it doesn't exists so we can reference it by
      // name consistently from from both the boda and caffe versions of the net.
      if( olp->type() == SoftmaxWithLoss_str ) {
	assert_st( add_bck_ops );
	assert_st( olp->top_size() <= 1 );
	if( olp->top_size() == 0 ) { olp->add_top(next_loss_node_name); }
	++nlnn_ix;
	next_loss_node_name = "loss_" + str(nlnn_ix);
      } else if( olp->type() == Softmax_str ) {
	assert_st( add_bck_ops );
	// here's where we'd convert from Softmax->SoftmaxWithLoss if we wanted to handle that
	rt_err( "unimplemented: reading caffe net with Softmax (not SoftmaxWithLoss) in add_bck_ops mode" );
      }
      bool layer_has_out_node = 0;
      for( int32_t i = 0; i != olp->top_size(); ++i ) {
	if( out_node_name == olp->top(i) ) { layer_has_out_node = 1; found_out_node = 1;}
      }
      // FIXME: in-place-op HACK/handling: if we're at the first layer *without* the desiered output node *after* seeing it prior,
      // 'unkeep' this layer and terminate. in particular, if there is an in-place operation on a node after it is first output,
      // this logic will keep it. however, the downside is that we can't *not* keep some subset of in-place layers on a given
      // node. sigh.
      if( found_out_node ) { if( !layer_has_out_node ) { --o; break; } } 
    }
    // FIXME? this is too strong now, and will be checked later -- but check something here? can't?
    // if( !found_out_node ) { rt_err( strprintf("run_cnet_t::create_net_param(): node out_node_name=%s not found as layer output in network\n", str(out_node_name).c_str() )); }
    while( net_param->layer_size() > o ) { net_param->mutable_layer()->RemoveLast(); }

    if( net_param->input_dim_size() == 0 ) { // if train-val form, convert to deploy form
      net_param->add_input(data_node_name);
      for( uint32_t i = 0; i != 4; ++i ) { net_param->add_input_dim(0); }
    }
    // FIXME: handle shape for input?
    assert_st( net_param->input_size() == 1 );
    assert_st( net_param->input_dim_size() == 4 );
    net_param->set_input_dim(0,in_num_imgs);
    net_param->set_input_dim(1,in_num_chans);
    net_param->set_input_dim(2,in_sz.d[1]);
    net_param->set_input_dim(3,in_sz.d[0]);
    // add label input if needed
    if( add_bck_ops ) {
      net_param->add_input(label_node_name);
      net_param->add_input_dim(in_num_imgs);
      net_param->add_input_dim(1);
      net_param->add_input_dim(1);
      net_param->add_input_dim(1);
    }

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

  // note; there is an unfortunate potential circular dependency here: we may need the pipe info
  // about the network before we have set it up if the desired size of the input image depends on
  // the net architeture (i.e. support size / padding / etc ).  currently, the only thing we need
  // the pipe for before setup is the number of images. this is determined by the blf_pack code
  // which needs the supports sizes and padding info from the pipe. but, since the pipe doesn't care
  // about the the number of input images (note: it does currently use in_sz to create the conv_ios
  // here, but that could be delayed), we can get away with creating the net_param first, then the
  // pipe, then altering num_input_images input_dim field of the net_param, then setting up the
  // net. hmm.
  void run_cnet_t::setup_cnet_param_and_pipe( void ) {
    assert( !net_param );
    create_net_param();
    conv_pipe = create_pipe_from_param( net_param, in_num_chans, out_node_name, add_bck_ops );
    // note: we may or may not need the trained blobs in the conv_pipe, depending on the compute
    // mode. but, in general, right now run_cnet_t does all needed setup for all compute modes all
    // the time ...
    p_net_param_t trained_net = must_read_binary_proto( trained_fn );
    copy_matching_layer_blobs_from_param_to_pipe( trained_net, net_param, conv_pipe );
    out_s = u32_ceil_sqrt( get_out_cio(0).chans );
    if( enable_upsamp_net ) { 
      assert_st( !add_bck_ops ); // not sensible?
      conv_pipe_upsamp = create_pipe_from_param( upsamp_net_param, in_num_chans, out_node_name, add_bck_ops ); 
      copy_matching_layer_blobs_from_param_to_pipe( trained_net, upsamp_net_param, conv_pipe_upsamp );
      create_upsamp_layer_weights( conv_pipe, net_param->layer(0).name(), 
				   conv_pipe_upsamp, upsamp_net_param->layer(0).name() ); // sets weights in conv_pipe_upsamp->layer_blobs
      assert_st( out_s == u32_ceil_sqrt( get_out_cio(1).chans ) ); // FIXME: too strong?
    }
    conv_pipe->calc_sizes_forward( in_sz, 0 ); 
    if( enable_upsamp_net ) { conv_pipe_upsamp->calc_sizes_forward( in_sz, 0 ); }

    assert_st( conv_fwd );
    assert_st( conv_fwd_upsamp );
    conv_fwd->init( conv_pipe, in_num_imgs );
    if( enable_upsamp_net ) { conv_fwd_upsamp->init( conv_pipe_upsamp, in_num_imgs ); } 

  }
  void run_cnet_t::setup_cnet_adjust_in_num_imgs( uint32_t const in_num_imgs_ ) {
    assert_st( net_param && conv_pipe );
    assert_st( net_param->input_dim_size() == 4 );
    in_num_imgs = in_num_imgs_;
    net_param->set_input_dim(0,in_num_imgs);
    if( enable_upsamp_net ) { upsamp_net_param->set_input_dim(0,in_num_imgs); } // FIXME/TODO: for now, run upsamp on all planes
  }

  void run_cnet_t::setup_cnet_net_and_batch( void ) {
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
	    u32_pt_t const feat_xy = {(uint32_t)bx, (uint32_t)by};
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
  // boda capture_classify --cnet-predict='(in_sz=600 600,ptt_fn=%(models_dir)/nin_imagenet_nopad/train_val.prototxt,trained_fn=%(models_dir)/nin_imagenet_nopad/best.caffemodel,out_node_name=cccp8)' --capture='(cap_res=640 480)'

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

#include"gen/caffeif.H.nesi_gen.cc"

}

