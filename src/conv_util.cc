// Copyright (c) 2013-2014, Matthew W. Moskewicz <moskewcz@alumni.princeton.edu>; part of Boda framework; see LICENSE
#include"boda_tu_base.H"
#include"conv_util.H"

#include"timers.H"
#include"str_util.H"
#include"has_main.H"
#include"has_conv_fwd.H"
#include"io_util.H"
#include"nesi.H"
#include"caffepb.H"

namespace boda 
{

  // type string checking + verify input/output argument count and other sanity checks
  bool conv_op_t::is( conv_op_info_t const & coi ) const { 
    if( type != coi.type ) { return 0; }
    if( !coi.num_tops.bnds_check_gele( tops.size() ) || !coi.num_bots.bnds_check_gele( bots.size() ) ) {
      rt_err( strprintf( "Wrong number of input or output arguments for operation of type '%s'. "
			 "had: tops.size()=%s bots.size()=%s, bnds are: coi.num_tops=%s coi.num_bots=%s\n", 
			 str(coi.type).c_str(), str(tops.size()).c_str(), str(bots.size()).c_str(), 
			 str(coi.num_tops).c_str(), str(coi.num_bots).c_str() ) );
    }
    return 1;
  }

  u32_pt_t conv_op_t::in_sz_to_out_sz( u32_pt_t const & in_sz, bool const ignore_padding ) const { 
    if( kern_sz.is_zeros() ) { // handle non-conv cases
      assert( !is(Convolution_coi) ); 
      if( is(Pooling_coi) || is(InnerProduct_coi) ) { return u32_pt_t{1,1}; } // global pooling / inner product special cases
      return in_sz; // otherwise, assume no effect on spatial dims (e.g. relu, lrn)
    }
    u32_pt_t const pad_in_sz = in_sz+(ignore_padding?u32_pt_t():in_pad.bnds_sum());
    if( !pad_in_sz.both_dims_ge(kern_sz) ) { return u32_pt_t(); } // padded input too small to create any output
    if( is(Convolution_coi) ) { return (pad_in_sz-kern_sz)/stride + u32_pt_t(1,1); }
    else if( is(Pooling_coi) ) { return ceil_div( pad_in_sz-kern_sz,stride ) + u32_pt_t(1,1); }
    else { rt_err("unknown layer type"); }
  }
  u32_pt_t conv_op_t::out_sz_to_in_sz( u32_pt_t const & out_sz, bool const ignore_padding ) const { 
    if( kern_sz.is_zeros() ) { // handle non-conv cases
      assert( !is(Convolution_coi) );
      if( is(Pooling_coi) || is(InnerProduct_coi) ) { // inner product and global pooling special cases
	if( out_sz != u32_pt_t{1,1} ) { rt_err( "global pooling layer can't produce an out_sz other than {1,1}" ); }
	return u32_pt_t{0,0};  // special value means all input will be used ...
      } else { // otherwise, assume no effect on spatial dims (e.g. relu, lrn)
        return out_sz;
      }
    } 
    assert( out_sz.both_dims_non_zero() ); // this seems like it would be hard/confusing to handle
    u32_pt_t const no_pad_in_sz =  kern_sz + (out_sz-u32_pt_t(1,1))*stride;
    if( ignore_padding ) { return no_pad_in_sz; }
    // if the following assert does not hold, the result would be
    // negative, indicating *no input* yields a larger out_sz than
    // requested (due to padding). this might be valid, but it's
    // unclear what to return (zero?), so for now we refuse to try.
    assert_st( no_pad_in_sz.both_dims_ge( in_pad.bnds_sum() ) ); 
    return no_pad_in_sz - in_pad.bnds_sum();
  }

  dims_t conv_pipe_t::get_data_img_dims( void ) const {
    if( data_img_node_names.size() != 1 ) { rt_err( "not exactly one data img input node in net; can't process. data img input nodes are: " + str(data_img_node_names) ); }
    return must_get_node( data_img_node_names[0] )->dims;
  }
  u32_pt_t conv_pipe_t::get_data_img_xy_dims_3_chans_only( void ) const {
    // FIXME: better errors here if named dims don't exist?
    dims_t const data_dims = get_data_img_dims();
    uint32_t const data_dims_chan = data_dims.dsz("chan");
    if( data_dims_chan != 3 ) { rt_err( "unsupported number of fata img input node chans; must == 3; saw '"+str(data_dims_chan)+"'" ); }
    return u32_pt_t{ data_dims.dsz("x"), data_dims.dsz("y") }; 
  }
  
  // if out_node_name is empty, this returns the single unique output node of the net or throws an error. if out_node_name is
  // non-empty, it returns the single output node of the layer with name out_node_name (or throws an error).
  p_conv_node_t conv_pipe_t::get_single_top_node( void ) const {
    if( out_node_name.empty() ) {
      if( tops.size() != 1 ) { rt_err( "not exactly one sink/output node in net; can't process. output nodes are: " + str(tops) ); }
      return must_get_node( *tops.begin() ); 
    } else {
      if( !has( *nodes, out_node_name ) ) { 
	rt_err( "node '"+out_node_name+"' specified for use as producing the primary net output not found in net." ); 
      }
      return must_get_node( out_node_name );
    }
  }
  
  p_conv_node_t conv_pipe_t::get_or_make_node( string const & name, bool const is_bot, bool const is_top ) {
    p_conv_node_t & ret = (*nodes)[name];
    if( !ret ) { ret.reset( new conv_node_t{name} ); tops.insert(name); bots.insert(name); }
    if( is_bot ) { tops.erase(name); } if( is_top ) { bots.erase(name); }
    return ret;
  }
  p_conv_node_t conv_pipe_t::must_get_node( string const & name ) const {
    map_str_p_conv_node_t::const_iterator i = nodes->find( name );
    assert_st( i != nodes->end() );
    return i->second;
  }
  p_conv_op_t conv_pipe_t::get_op( string const & name ) const {
    map_str_p_conv_op_t::const_iterator i = convs->find( name );
    assert_st( i != convs->end() );
    return i->second;
  }
  void conv_pipe_t::add_conv( p_conv_op_t const & conv ) {
    bool in_place = 0;
    if( conv->is(ReLU_coi) || conv->is(Dropout_coi) || conv->is(ZeroIfNeg_coi) ) { 
      if( conv->is(ZeroIfNeg_coi) ) { assert_st( conv->tops[0] == conv->bots[0] ); }
      else { assert_st( conv->tops == conv->bots ); }
      get_or_make_node(conv->bots[0], 0, 0 )->in_place_ops.push_back( conv );
      in_place = 1;
    }
    bool did_ins = convs->insert( make_pair( conv->tag, conv ) ).second;
    if( !did_ins ) { rt_err( strprintf( "duplicate conv op '%s' seen; can't process net", conv->tag.c_str() ) ); }
    if( in_place ) { return; } // don't add in-place ops to top_for and bot_for
    for( vect_string::const_iterator i = conv->tops.begin(); i != conv->tops.end(); ++i ) {
      get_or_make_node( *i, 0, 1 )->top_for.push_back( conv->tag );
    }
    for( vect_string::const_iterator i = conv->bots.begin(); i != conv->bots.end(); ++i ) {
      get_or_make_node( *i, 1, 0 )->bot_for.push_back( conv->tag );
    }
  }

  // if the node has one top_for (a single writer), return it. if it has no writers, return null.
  // otherwise, throw an error.
  p_conv_op_t conv_pipe_t::maybe_get_single_writer( p_conv_node_t const & node ) const {
    if( node->top_for.empty() ) { return p_conv_op_t(); }
    if( node->top_for.size() != 1 ) { 
      rt_err( "unhandled multiple writers for node: " + node->name ); 
    }
    return get_op( node->top_for[0] );
  }
  p_conv_op_t conv_pipe_t::get_single_writer( p_conv_node_t const & node ) const {
    p_conv_op_t ret = maybe_get_single_writer( node );
    if( !ret ) { rt_err( "unhandled no writer (i.e. was primary input) for node: " + node->name ); }
    return ret;
  }

  // if the op has one input, return maybe_get_single_writer() for than
  // input. otherwise throw an error.
  p_conv_op_t conv_pipe_t::maybe_get_single_parent( p_conv_op_t const & cop ) const {
    assert_st( !cop->bots.empty() );
    if( cop->bots.size() != 1 ) {
      printf( "WARNING: unhandled multi-input op in support calc, using first input. cop->bots=%s\n", str(cop->bots).c_str() );
    }
    return maybe_get_single_writer( must_get_node(cop->bots[0]) );
  }


  void conv_pipe_t::calc_support_forward_op( p_conv_op_t const & cop, bool const ignore_padding ) {
    assert_st( cop->tops.size() >= 1 );
    p_conv_node_t const & node_out = must_get_node(cop->tops[0]);
    conv_support_info_t & csi_out = node_out->csi;
    if( csi_out.valid() ) { rt_err( "unhandled: node with multiple writers:"+node_out->name ); }

    // FIXME?: for now, we don't try to calculate support info for bck operations 
    if( cop->is( BckConv_coi ) ) { // { in, filts, biases, out_grad_loss } --> { in_grad_loss, filts_grad_loss, biases_grad_loss }
    } else if( cop->is( Spreading_coi ) ) { 
    } else if( cop->is( SoftmaxWithLoss_coi ) ) { 
      csi_out.support_stride = u32_pt_t{};
      assert_st( cop->in_pad.is_zeros() ); csi_out.eff_tot_pad = must_get_node(cop->bots[0])->csi.eff_tot_pad;
      assert_st( cop->out_chans == 0 ); 
      p_conv_node_t const & loss_node = must_get_node( cop->tops[1] );
      loss_node->csi.support_sz = u32_pt_t{};
      loss_node->csi.eff_tot_pad = csi_out.eff_tot_pad; // FIXME: correct? needed? maybe set to bogus/sentinel value?
    } else {    
      for( vect_string::const_iterator j = cop->bots.begin(); j != cop->bots.end(); ++j ) {
	p_conv_node_t const & j_node = must_get_node(*j);
	conv_support_info_t const & csi_in = j_node->csi;
	if( cop->is( Concat_coi ) ) {
	  assert_st( cop->has_one_top() );
	  if( (j == cop->bots.begin()) || (csi_in.support_stride.dims_max() > csi_out.support_stride.dims_max()) ) { // first input or bigger stride
	    if( j != cop->bots.begin() ) { 
	      printf( "WARNING: unhandled Concat layer '%s' with different strided inputs. "
		      "Note: support will be max size over inputs with largest stride in any dim.\n", str(cop->bots).c_str() );
	    }
	    csi_out.support_stride = csi_in.support_stride;
	    csi_out.support_sz = csi_in.support_sz;
	  } else { 
	    if( csi_in.support_stride == csi_out.support_stride ) { csi_out.support_sz.max_eq( csi_in.support_sz ); }
	  }
	  csi_out.eff_tot_pad.max_eq( csi_in.eff_tot_pad );
	  assert( !cop->out_chans ); // concat shouldn't have a # of output chans specified
	} else {
	  if( j == cop->bots.begin() ) {
	    assert_st( cop->has_one_top() );
	    u32_pt_t const in_sz_1x1 = cop->out_sz_to_in_sz( u32_pt_t(1,1), ignore_padding ); // == cop.kern_sz (if ign_pad)
	    if( in_sz_1x1.is_zeros() || csi_in.support_sz.is_zeros() )  { // special values that means use all input
	      csi_out.support_sz = u32_pt_t{};
	    } else {
	      assert_st( in_sz_1x1.both_dims_non_zero() );
	      csi_out.support_sz = csi_in.support_sz + ( in_sz_1x1 - u32_pt_t(1,1) )*csi_in.support_stride;
	    }
	    assert_st( cop->stride.both_dims_non_zero() );
	    csi_out.support_stride = csi_in.support_stride*cop->stride;
	    csi_out.eff_tot_pad = csi_in.eff_tot_pad + cop->in_pad.scale_dims( csi_in.support_stride );
	  } else { rt_err( "unhandled multi-input operation: "+cop->tag+" of type " + cop->type+" " ); }
	}
      }
    }
  }
  void conv_pipe_t::calc_support_forward_rec( string const & node_name, bool const ignore_padding ) {
    p_conv_node_t const & node = must_get_node( node_name );
    // propogate support info forward from node to all ops that it feeds and thier outputs
    for( vect_string::const_iterator i = node->bot_for.begin(); i != node->bot_for.end(); ++i ) {
      p_conv_op_t const & cop = get_op( *i );
      if( !cop->on_seen_bot() ) { continue; } // wait till we've seen all bottoms
      calc_support_forward_op( cop, ignore_padding );
      // depth-first recursive processing for any outputs
      for( vect_string::const_iterator i = cop->tops.begin(); i != cop->tops.end(); ++i ) { calc_support_forward_rec( *i, ignore_padding ); }
    }
  }
  // generally more sensible to with ignore_padding_for_support = 1 (but possibly interesting if = 0 too)
  void conv_pipe_t::calc_support_info( bool const ignore_padding ) {
    // support info for all root inputs should already be set by data layers. if not, it's a fatal error.
    vect_string unhandled_inputs;
    for( set_string::const_iterator i = bots.begin(); i != bots.end(); ++i ) { 
      p_conv_node_t const & node = must_get_node( *i );
      conv_support_info_t & csi = node->csi;
      if( !csi.valid() ) { unhandled_inputs.push_back( *i ); }
    }
    if( !unhandled_inputs.empty() ) {
      rt_err( "calc_support_info(): had unhandled/unknown inputs (not data layer output, not filts/biases): " + str(unhandled_inputs) ); 
    }
    topo_visit_setup();
    for( set_string::const_iterator i = bots.begin(); i != bots.end(); ++i ) {  calc_support_forward_rec( *i, ignore_padding ); }
  }

  void conv_pipe_t::calc_dims_op( p_conv_op_t const & cop ) {
    assert_st( cop->tops.size() >= 1 );
    p_conv_node_t const & node_out = must_get_node(cop->tops[0]);
    dims_t & dims_out = node_out->dims;
    if( dims_out.size() ) { rt_err( "calc_dims_op(): unhandled: out dims already set (node with multiple writers):" + node_out->name ); }
    
    if( cop->is( BckConv_coi ) ) { // { in, filts, biases, out_grad_loss } --> { in_grad_loss, filts_grad_loss, biases_grad_loss }
      printf( "cop->tag=%s cop->tops=%s cop->bots=%s\n", str(cop->tag).c_str(), str(cop->tops).c_str(), str(cop->bots).c_str() );
      for( uint32_t i = 0; i != 1; ++i ) { // propogate # chans
	dims_t & od = must_get_node(cop->tops[i])->dims;
	if( od.size() ) { rt_err( "calc_dims_op(): unhandled: out dims already set (node with multiple writers):" + cop->tops[i] ); }
	od = must_get_node(cop->bots[i])->dims;
      }
    } else if( cop->is( Spreading_coi ) ) { 
      assert_st( cop->out_chans == 0 ); 
      dims_out = must_get_node(cop->bots[2])->dims;
      // FIXME?: for now, we don't try to calculate support info for bck operations 
    } else if( cop->is( SoftmaxWithLoss_coi ) ) { 
      dims_out = must_get_node(cop->bots[0])->dims;
      dims_t & loss_dims = must_get_node( cop->tops[1] )->dims;
      // loss is a singleton (no img or chan dims anyway)... but, FIXME: why are there exactly 2 spatial dims? what else could you put? just 'x'?
      loss_dims = dims_t( vect_uint32_t{1,1}, vect_string{"y","x"}, 1 ); 
      // FIXME: even though the label is an input, we currently can't/don't try to set it's dims intially (i.e. from the data
      // layer), although perhaps that would make more sense. instead, we allow it to be set here, but check that it is
      // correct if it is already set. if it ever is set 'feed forward', this check is still good/okay. however, if it
      // is used by other things as an input, and they expect it to be set (i.e. becuase they use it), then that's no
      // good -- it might or might not get randomly set here depending on traversal order. really it's just not
      // generally okay to set it here.
      dims_t implied_label_dims( vect_uint32_t{ dims_out.dsz("img"), dims_out.dsz("y"), dims_out.dsz("x") }, vect_string{ "img", "y", "x" }, 1 );
      dims_t & label_dims = must_get_node( cop->bots[1] )->dims;
      if( label_dims.empty() ) { label_dims = implied_label_dims; }
      else if( label_dims != implied_label_dims ) { rt_err( "error: label used by multiple SoftmaxWithLoss layers with differing xy size or # imgs" ); }

      uint32_t & label_max_val = must_get_node( cop->bots[1] )->cio.max_val;
      uint32_t const implied_label_max_val = dims_out.dsz("chan");
      if( label_max_val == 0 ) { label_max_val = implied_label_max_val; }
      if( label_max_val != implied_label_max_val  ) { rt_err( "error: label used by multiple SoftmaxWithLoss layers with differing #s of chans." ); }

    } else if( cop->is( Concat_coi ) ) {
      assert_st( cop->has_one_top() ); 
      uint32_t dims_out_chans = 0; // start at zero for concat layer accumulation across inputs case
      for( vect_string::const_iterator j = cop->bots.begin(); j != cop->bots.end(); ++j ) {
	dims_t const & j_dims = must_get_node(*j)->dims;
	dims_out_chans += j_dims.dsz("chan"); // sum chans across all inputs
	if( !dims_out.size() ) { dims_out = j_dims; dims_out.clear_strides(); dims_out.must_get_dim_by_name("chan").sz = 0; } // convert to template
	else if( !j_dims.matches_template( dims_out ) ) { 
	  rt_err( "concat layer had incompatible inputs; must have all same non-chan dims. template (from first input) was: " + 
		  str(dims_out) + ". mismatching input was (index="+str(j - cop->bots.begin())+"): " + str(j_dims) );
	}
      }
      dims_out.must_get_dim_by_name("chan").sz = dims_out_chans;
      dims_out.calc_strides();
    } else {    
      assert_st( cop->has_one_top() );
      if( cop->bots.size() != 1 ) { rt_err( "calc_dims(): unhandled multi-input operation: "+cop->tag+" of type " + cop->type+" " ); }
      dims_t const & dims_in = must_get_node(cop->bots[0])->dims;
      assert_st( cop->stride.both_dims_non_zero() ); // FIXME: still belongs here? handled in in_sz_to_out_sz?
      dims_out = dims_in; // starting point
      dims_out.must_get_dim_by_name("chan").sz = cop->out_chans ? cop->out_chans : dims_in.dsz("chan"); // reset or propogate num_chans
      u32_pt_t const dims_out_sz = cop->in_sz_to_out_sz( get_xy_dims( dims_in ), 0 );

      if( dims_out_sz.both_dims_non_zero() ) { // calculate used_sz for debugging/informational output in dump_ios()
	must_get_node(cop->bots[0])->cio.used_sz.max_eq( cop->out_sz_to_in_sz( dims_out_sz, 0 ) ); 
      } // else if there's no output, we used no input (used_sz left at zero)
      
      set_xy_dims( dims_out, dims_out_sz );
      dims_out.calc_strides();
    }
    for( vect_string::const_iterator i = cop->tops.begin(); i != cop->tops.end(); ++i ) { calc_dims_rec( *i ); }
  }
  void conv_pipe_t::calc_dims_rec( string const & node_name ) {
    p_conv_node_t const & node = must_get_node( node_name );
    for( vect_string::const_iterator i = node->bot_for.begin(); i != node->bot_for.end(); ++i ) {
      p_conv_op_t const & cop = get_op( *i );
      if( !cop->on_seen_bot() ) { continue; } // wait till we've seen all bottoms
      calc_dims_op( cop );
    }
  }
  void conv_pipe_t::calc_dims( void ) {
    topo_visit_setup(); 
    for( set_string::const_iterator i = bots.begin(); i != bots.end(); ++i ) {  calc_dims_rec( *i ); }  
    for( map_str_p_conv_node_t::const_iterator i = nodes->begin(); i != nodes->end(); ++i ) { 
      dims_t const & d = i->second->dims;
      //printf( "post calc_dims() %s dims: %s\n", i->first.c_str(), str(d).c_str() );
      if( d.empty() ) { rt_err( strprintf( "error: no dims calculated for node %s after calc_dims()", str(i->first).c_str() ) ); }
    }
  }
  
  void conv_pipe_t::clear_sizes( void ) {
    for( map_str_p_conv_node_t::iterator i = nodes->begin(); i != nodes->end(); ++i ) { i->second->cio = conv_io_t(); }
  }
  void conv_pipe_t::topo_visit_setup( void ) {
    for( map_str_p_conv_op_t::iterator i = convs->begin(); i != convs->end(); ++i ) { i->second->seen = 0; }
  }

  void conv_pipe_t::calc_sizes_forward_op( p_conv_op_t const & cop, bool const ignore_padding ) {
    assert_st( cop->tops.size() >= 1 );
    p_conv_node_t const & node_out = must_get_node(cop->tops[0]);
    conv_io_t & cio_out = node_out->cio;
    if( !cio_out.sz.is_zeros() ) { rt_err( "node size calculation is not supported for reconvegent networks at node:"+node_out->name ); }

    if( cop->is( BckConv_coi ) ) { 
      for( uint32_t i = 0; i != 1; ++i ) { // propogate sizes
	u32_pt_t & osz = must_get_node(cop->tops[i])->cio.sz;
	if( !osz.is_zeros() ) { rt_err( "node size calculation is not supported for reconvegent networks at node:"+node_out->name ); }
	osz = must_get_node(cop->bots[i])->cio.sz; 
      }
    } else if( cop->is( ZeroIfNeg_coi ) ) { 
      cio_out.sz = must_get_node(cop->bots[0])->cio.sz;
    } else if( cop->is( Spreading_coi ) ) { 
      cio_out.sz = must_get_node(cop->bots[2])->cio.sz; // in_grad_loss output is same size as in
    } else if( cop->is( SoftmaxWithLoss_coi ) ) { 
      cio_out.sz = must_get_node(cop->bots[0])->cio.sz;
      conv_io_t & loss_cio = must_get_node( cop->tops[1] )->cio;
      loss_cio.sz = u32_pt_t{1,1}; // loss is a singleton
      //loss_cio.per_batch = 1;
    } else {
      assert_st( cop->has_one_top() );
      if( (cop->bots.size() != 1) && !cop->is(Concat_coi) ) { 
	rt_err( "unhandled multi-input operation: "+cop->tag+" of type " + cop->type+" " ); }
      for( vect_string::const_iterator j = cop->bots.begin(); j != cop->bots.end(); ++j ) {
	conv_io_t const & cio_in = must_get_node(*j)->cio;
	if( j == cop->bots.begin() ) { // first input 
	  cio_out.sz = cop->in_sz_to_out_sz( cio_in.sz, ignore_padding );
	} else { // handle multiple inputs for concat layer (only!)
	  assert( cop->is( Concat_coi ) );
	  // x/y dims must agree across all inputs
	  u32_pt_t const out_sz = cop->in_sz_to_out_sz( cio_in.sz, ignore_padding );
	  assert_st( out_sz == cio_out.sz );
	}
      }
    }
    for( vect_string::const_iterator i = cop->tops.begin(); i != cop->tops.end(); ++i ) { calc_sizes_forward_rec( *i, ignore_padding ); }
  }
  void conv_pipe_t::calc_sizes_forward_rec( string const & node_name, bool const ignore_padding ) {
    p_conv_node_t const & node = must_get_node( node_name );
    // propogate support info forward from node to all ops that it feeds and thier outputs
    for( vect_string::const_iterator i = node->bot_for.begin(); i != node->bot_for.end(); ++i ) {
      p_conv_op_t const & cop = get_op( *i );
      if( !cop->on_seen_bot() ) { continue; } // wait till we've seen all bottoms
      calc_sizes_forward_op( cop, ignore_padding );
    }
  }


  // we consider a node a 'label' if it is the second input to one or more SoftmaxWithLoss ops (and no other ops). if it
  // is, we a representtive of the other input(s) of the SoftmaxWithLoss layers (or an error if they
  // disagree in size or chans).  otherwise, we return 0.
  p_conv_node_t conv_pipe_t::get_fwd_top_for_label( string const & n ) const {
    p_conv_node_t const & node = must_get_node( n );
    bool has_non_sm_uses = 0;
    p_conv_node_t rep_fwd_top;
    for( vect_string::const_iterator i = node->bot_for.begin(); i != node->bot_for.end(); ++i ) {
      p_conv_op_t const & cop = get_op( *i );
      if( cop->is( SoftmaxWithLoss_coi ) ) { 
	p_conv_node_t fwd_top = must_get_node( cop->bots[0] );
	if( rep_fwd_top ) { 
	  if( rep_fwd_top->cio.sz != fwd_top->cio.sz ) {
	    rt_err( "error: label used by multiple SoftmaxWithLoss layers with differing xy size." );
	  }
	}
	if( !rep_fwd_top ) { rep_fwd_top = fwd_top; }
      }
      else { has_non_sm_uses = 1; }
    }
    if( rep_fwd_top && has_non_sm_uses ) { rt_err( "unhandled: input node '" + n + "' used by SoftmaxWithLoss *and* other layer types" ); }
    return rep_fwd_top;
  }

  void conv_pipe_t::calc_sizes_forward( bool const ignore_padding ) {
    // size info for all non-label root inputs should already be set
    for( set_string::const_iterator i = bots.begin(); i != bots.end(); ++i ) { 
      conv_io_t & cio = must_get_node( *i )->cio;
      p_conv_node_t fwd_top = get_fwd_top_for_label( *i );
      if( !fwd_top ) { // we calculate the sizes of labels later
	if( cio.sz.is_zeros() ) { 
	  rt_err( "calc_sizes_forward(): unhandled non-label input '"+(*i)+"' with unknown size. "
		  "internal error, since error should have been caught in calc_support_size()?" ); 
	}
      }
    }
    topo_visit_setup();
    for( set_string::const_iterator i = bots.begin(); i != bots.end(); ++i ) { calc_sizes_forward_rec( *i, ignore_padding ); }
    for( set_string::const_iterator i = bots.begin(); i != bots.end(); ++i ) { 
      conv_io_t & cio = must_get_node( *i )->cio;
      p_conv_node_t fwd_top = get_fwd_top_for_label( *i );
      if( fwd_top ) { 
	assert( cio.sz.is_zeros() ); // shouldn't be calculated yet
	cio.sz = fwd_top->cio.sz;
      } 
      assert_st( !cio.sz.is_zeros() ); // all bot sizes (and further-but-unchecked-here, all nodes) should be set
    } 
  }

  // note: assumed to be called after sizes are set by set_dims(). overwrites the xy_dims for nodes it touches.
  // note: recursively sturctured, but only works for chains currently. it's unclear what the
  // extention to non-chains would be exactly, but it would seem to depend on handling some
  // particular type of conv_op with >1 input.
  void conv_pipe_t::calc_sizes_back_rec( p_conv_node_t const & node_out, bool const ignore_padding ) {
    u32_pt_t const & xy_dims_out = get_xy_dims( node_out->dims );
    p_conv_op_t cop = maybe_get_single_writer( node_out );
    if( !cop ) { return; } // reached source, done
    assert_st( cop->has_one_top_one_bot() );
    p_conv_node_t node_in = must_get_node(cop->bots[0]);
    u32_pt_t xy_dims_in = get_xy_dims( node_in->dims );
    if( xy_dims_in.is_zeros() ) { rt_err( "internal error: !cio_in.valid() in calc_sizes_back_rec() at node:"+node_out->name ); }
    if( !xy_dims_out.both_dims_non_zero() ) {
      rt_err( strprintf( "calc_sizes_back(): unhandled/questionable case: pipeline stage %s output is zero-area.",
			 cop->tag.c_str() ) );
    }
    xy_dims_in = cop->out_sz_to_in_sz( xy_dims_out, ignore_padding );
    node_in->cio.used_sz = xy_dims_in; // by semantics of out_sz_to_in_sz (but checked below)
    assert_st( xy_dims_out == cop->in_sz_to_out_sz( xy_dims_in, ignore_padding ) );
    set_xy_dims( node_in->dims, xy_dims_in );
    calc_sizes_back_rec( node_in, ignore_padding ); // depth-first recursive processing for the input
  }

  void conv_pipe_t::calc_sizes_back( u32_pt_t const & out_sz, bool const ignore_padding ) {
    // initialize support info for single output
    p_conv_node_t const & node = get_single_top_node();
    u32_pt_t xy_dims_in = get_xy_dims( node->dims );
    assert( !xy_dims_in.is_zeros() );
    xy_dims_in = out_sz;
    set_xy_dims( node->dims, xy_dims_in );
    calc_sizes_back_rec( node, ignore_padding ); // calculate support
  }

  void conv_pipe_t::dump_pipe_rec( std::ostream & out, string const & node_name ) {
    p_conv_node_t node = must_get_node( node_name );
    if( node->bot_for.size() > 1 ) { 
      out << strprintf("node used by multiple ops:" ); 
      for( vect_string::const_iterator i = node->bot_for.begin(); i != node->bot_for.end(); ++i ) { out << " " << *i; }
      out << strprintf("\n");
    }
    conv_support_info_t const & csi = node->csi;
    out << strprintf( "support_sz=%s support_stride=%s eff_tot_pad=%s\n", 
		      str(csi.support_sz).c_str(), 
		      str(csi.support_stride).c_str(), str(csi.eff_tot_pad).c_str() );
    for( vect_string::const_iterator i = node->bot_for.begin(); i != node->bot_for.end(); ++i ) {
      p_conv_op_t const & cop = get_op( *i );
      if( !cop->on_seen_bot() ) { continue; } // wait till we've seen all bottoms
      out << strprintf( "    ----  conv=%s \n", str(*cop).c_str() );
      for( vect_string::const_iterator i = cop->tops.begin(); i != cop->tops.end(); ++i ) { dump_pipe_rec( out, *i ); }
    }
  }

  void conv_pipe_t::dump_pipe( std::ostream & out ) {
    out << strprintf( "== BEGIN CONV PIPE ==\n" );
    topo_visit_setup();
    for( set_string::const_iterator i = bots.begin(); i != bots.end(); ++i ) { dump_pipe_rec( out, *i ); }
    out << strprintf( "== END CONV PIPE ==\n" );
  }

  void conv_pipe_t::dump_ios_rec( std::ostream & out, string const & node_name ) {
    p_conv_node_t node = must_get_node( node_name );
    if( node->bot_for.size() > 1 ) { 
      out << strprintf("(-->" ); 
      for( vect_string::const_iterator i = node->bot_for.begin(); i != node->bot_for.end(); ++i ) { out << " " << *i; }
      out << strprintf(")");
    }
    u32_pt_t const & used_sz = node->cio.used_sz;
    u32_pt_t const xy_sz = get_xy_dims( node->dims );
    out << strprintf( "sz=%s -> ", str(xy_sz).c_str() );
    string size_err;
    if( xy_sz != used_sz ) { 
      if( (used_sz.d[0] > xy_sz.d[0]) || (used_sz.d[1] > xy_sz.d[1]) ) { size_err += "IMPLICIT PAD; "; }
      if( (used_sz.d[0] < xy_sz.d[0]) || (used_sz.d[1] < xy_sz.d[1]) ) { size_err += "DATA DISCARDED; "; }
      out << strprintf( "[%sused_sz=%s] -> ", size_err.c_str(), str(used_sz).c_str() );
    }
    for( vect_string::const_iterator i = node->bot_for.begin(); i != node->bot_for.end(); ++i ) {
      p_conv_op_t const & cop = get_op( *i );
      if( !cop->on_seen_bot() ) { continue; } // wait till we've seen all bottoms
      if( cop->tops.size() == 1 ) {
	out << cop->tag << " -> ";
	dump_ios_rec( out, cop->tops[0] );
      } else {
	out << cop->tag << " (";
	for( uint32_t i = 0; i != cop->tops.size(); ++i ) {
	  out << cop->tag << " -> ";
	  dump_ios_rec( out, cop->tops[i] );
	  out << cop->tag << ",";
	}
	out << cop->tag << " )";
      }
    }
  }
  void conv_pipe_t::dump_ios( std::ostream & out ) {
    out << "CONV_IOS: ";
    topo_visit_setup();
    for( set_string::const_iterator i = bots.begin(); i != bots.end(); ++i ) { dump_ios_rec( out, *i ); }
    out << "\n";
  }

  void print_blob_decl( std::ostream & out, string const & bn, p_conv_node_t const & node ) {
    string isss;
    if( node->top_for.empty() ) { isss += " SOURCE"; }
    if( node->bot_for.empty() ) { isss += " SINK"; }
    dims_t const & dims = node->dims;
    out << strprintf( "net.ndas[\"%s\"] = NDA(\"%s\"", bn.c_str(), bn.c_str() );
    for( uint32_t i = 0; i != dims.size(); ++i ) { out << "," << dims[i].sz; }
    out << ") #" << isss << " ";
    for( uint32_t i = 0; i != dims.size(); ++i ) { if( i ) { out << ","; } out << dims[i].name; }
    out << "\n";
  }
  
  string get_conv_as_sgemm( string const & top_name, string const & bot_name, string const & filts_name,
			    uint32_t const M, uint32_t const N, uint32_t const K, string const & extra_params ) {
    // FIXME: predates sz->dims conversion -- can't use num_img in pyIR like this anymore, need to use dims.dsz("img")
    // at this level and emit num_imgs as constant
    string const buf_name = bot_name + "_one_row_per_patch_buf";
    string ret;
    ret += strprintf( "net.ndas[\"%s\"] = NDA(\"%s\",%u,%u)\n",buf_name.c_str(),buf_name.c_str(),M,N);
    ret += strprintf( "for i in range(0,num_img):\n" );
    ret += strprintf( "  patches_to_rows( src=%s[i,:,:,:], dest=%s, %s ) # one copy per output elem\n",
		      bot_name.c_str(),buf_name.c_str(), extra_params.c_str() );
    ret += strprintf( "  %s = %s * transpose(reshape(%s,%u,%u)) # sgemm: MxNxK == %ux%ux%u\n", top_name.c_str(),buf_name.c_str(), 
		      filts_name.c_str(),K,N,M,N,K );
    return ret;
  }

  void print_op_decl( std::ostream & out, conv_pipe_t const * const pipe, p_conv_op_t const & cop, bool const expanded_ops ) {
    string extra_params;
    string expanded_op;
    char const * const tag_id = cop->tag.c_str();
    
    string const pad_and_stride = strprintf( "in_pad=\"%s\",stride=\"%s\"", cop->in_pad.parts_str().c_str(), str(cop->stride).c_str() );
    uint32_t M = 0, N = 0, K = 0;
    if( cop->is( Convolution_coi ) || cop->is( InnerProduct_coi ) ) {
      dims_t const & in_dims = pipe->must_get_node( cop->bots[0] )->dims;
      uint32_t const in_chans = in_dims.dsz("chan");
      u32_pt_t kern_sz = cop->kern_sz;
      if( kern_sz.is_zeros() ) { kern_sz = get_xy_dims( in_dims ); } // 'global' input special case

      out << strprintf( "net.ndas[\"%s_filts\"] = NDA(\"%s_filts\",%s,%s,%s,%s) # SOURCE out_chan,in_chan,y,x\n", 
			tag_id, tag_id, str(cop->out_chans).c_str(), str(in_chans).c_str(),
			str(kern_sz.d[1]).c_str(), str(kern_sz.d[0]).c_str() );
      out << strprintf( "net.ndas[\"%s_biases\"] = NDA(\"%s_biases\",%s) # SOURCE out_chan\n", 
			tag_id, tag_id, str(cop->out_chans).c_str() );
      extra_params = strprintf( ",filts_name=\"%s_filts\",biases_name=\"%s_biases\"", tag_id, tag_id );

      // see FIXME in get_conv_as_sgemm() ...
      assert_st( cop->tops.size() == 1 );
      M = get_xy_dims( pipe->must_get_node( cop->tops[0] )->dims ).dims_prod();
      N = kern_sz.d[0]*kern_sz.d[1]*in_chans;
      K = cop->out_chans;
      expanded_op = get_conv_as_sgemm(cop->tops[0],cop->bots[0],cop->tag+"_filts",M,N,K,pad_and_stride);
    }
    // print decls for all of this ops output nodes here
    for( vect_string::const_iterator i = cop->tops.begin(); i != cop->tops.end(); ++i ) {
      print_blob_decl( out, *i, pipe->must_get_node(*i) ); 
    }
    // print acutal op
    if( expanded_ops && !expanded_op.empty() ) { out << expanded_op; }
    else {
      out << strprintf( "%s(name=\"%s\",bot_names=%s,top_names=%s%s,\n\t%s)\n", 
			cop->type.c_str(), tag_id, as_py_str_list(cop->bots).c_str(), as_py_str_list(cop->tops).c_str(),
			extra_params.c_str(), pad_and_stride.c_str() );
    }
  }

  void conv_pipe_t::dump_ops_rec( std::ostream & out, string const & node_name, bool const & expand_ops ) {
    p_conv_node_t node = must_get_node( node_name );
    // print source nodes here, otherwise print with thier writing op
    if( node->top_for.empty() ) { print_blob_decl( out, node_name, node ); }
    else { assert( node->top_for.size() == 1 ); } // multiple writers not handled
    // print in-place ops for this node
    for( vect_p_conv_op_t::const_iterator j = node->in_place_ops.begin(); j != node->in_place_ops.end(); ++j ) {
      p_conv_op_t const & ip_cop = *j;
      out << strprintf( "%s(name=\"%s\",in_place=[\"%s\"])\n", ip_cop->type.c_str(), ip_cop->tag.c_str(), node->name.c_str() );
    }
    for( vect_string::const_iterator i = node->bot_for.begin(); i != node->bot_for.end(); ++i ) {
      p_conv_op_t const & cop = get_op( *i );
      if( !cop->on_seen_bot() ) { continue; } // wait till we've seen all bottoms
      print_op_decl( out, this, cop, expand_ops );
      for( vect_string::const_iterator j = cop->tops.begin(); j != cop->tops.end(); ++j ) { dump_ops_rec( out, *j, expand_ops ); }
    }
  }
  void conv_pipe_t::dump_ops( std::ostream & out, bool const & expand_ops ) {
    topo_visit_setup();
    for( set_string::const_iterator i = bots.begin(); i != bots.end(); ++i ) { dump_ops_rec( out, *i, expand_ops ); }
  }

  // running test case for add_bck_ops/gradient calculations:
  // boda test_compute --model-name=nin_imagenet --wins-per-image=1 --imgs='(pil_fn=%(boda_test_dir)/pascal/head_1/%%s.txt)' --run-cnet='(in_dims=(img=1),out_node_name=conv1_grad_loss,add_bck_ops=1)' --cf2="(mode=rtc,show_rtc_calls=0,per_call_fn=out.py,dump_vars=())" --max-err=2 && cat test_compute.txt

  void conv_pipe_t::add_bck_ops_op( p_conv_op_t const & cop ) {
    if( cop->is( Softmax_coi ) ) { assert_st(0 ); }
    else if( cop->is( SoftmaxWithLoss_coi ) ) {
      assert_st( cop->bots[0]+"_grad_loss" == cop->tops[0] );
    } else if( cop->is( Pooling_coi ) ) {
      p_conv_op_t bcop( new conv_op_t );
      *bcop = *cop;
      bcop->type = Spreading_coi.type;
      bcop->tag += "_bck";
      swap( bcop->tops, bcop->bots );
      bcop->bots.push_back( bcop->bots[0] + "_grad_loss" );
      bcop->bots.push_back( bcop->tops[0] ); // take original input as input (need size and which-elem-is-max per window) could use mask instead)
      bcop->tops[0] += "_grad_loss"; // note: pooling has no params, so there is second output for parameter gradients (as with some bck ops)
      if( !has( *nodes, bcop->bots[1] ) ) { printf( "FIXME: missing bot: bcop->bots[1]=%s -- op dropped.\n", str(bcop->bots[1]).c_str() ); }
      else { add_conv( bcop ); }
    } else if( cop->is( ReLU_coi ) ) {
      p_conv_op_t bcop( new conv_op_t );
      *bcop = *cop;
      bcop->type = ZeroIfNeg_coi.type;
      bcop->tag += "_bck";
      swap( bcop->tops, bcop->bots );
      bcop->bots.push_back( bcop->tops[0] ); // take original input as input
      bcop->bots[0] += "_grad_loss";
      bcop->tops[0] += "_grad_loss"; // note: ReLU has no params, so there is second output for parameter gradients (as with some bck ops)
      if( !has( *nodes, bcop->bots[0] ) ) { printf( "FIXME: missing bot: bcop->bots[0]=%s -- op dropped.\n", str(bcop->bots[0]).c_str() ); }
      else { add_conv( bcop ); }
    } else if( cop->is( Convolution_coi ) ) {
      p_conv_op_t bcop( new conv_op_t );
      *bcop = *cop;
      bcop->type = BckConv_coi.type;
      // currently, the 'regular' fwd conv has implicit filts/biases inputs. for now, we're going to make them explicit
      // here for BckConv. see TODO item. FIXME: note dup'd code with rtc_fwd.cc
      //bcop->bots.push_back( bcop->tag + "_filts" ); // FIXME_EFB
      //bcop->bots.push_back( bcop->tag + "_biases" ); // FIXME_EFB
      bcop->bots.push_back( bcop->tops[0] + "_grad_loss" ); // take _grad_loss of fwd conv output as input as well
      bcop->tops.clear(); for( uint32_t i = 0; i != 1; ++i ) { bcop->tops.push_back( bcop->bots[i] + "_grad_loss" ); } // outputs grads
      bcop->tag += "_bck";
      if( !has( *nodes, bcop->bots[1] ) ) { printf( "FIXME: BckConv: missing bot: bcop->bots[3]=%s -- op dropped.\n", str(bcop->bots[1]).c_str() ); }
      else { 
	//printf( "FIXME_EFB: not adding BckConv: cop->tag=%s\n", str(cop->tag).c_str() );
	add_conv( bcop ); 
      }
    } else {
      printf( "FIXME: add_bck_ops: unhandled cop->type=%s\n", str(cop->type).c_str() );
    }
  }
  void conv_pipe_t::add_bck_ops_rec( string const & node_name ) {
    p_conv_node_t node = must_get_node( node_name );
    for( vect_p_conv_op_t::const_reverse_iterator j = node->in_place_ops.rbegin(); j != node->in_place_ops.rend(); ++j ) {
      p_conv_op_t const & ip_cop = *j;
      // FIXME: handle bck for in_place_opts. note: as usual, in_place_ops seem to be problematic or at least special. 
      add_bck_ops_op( ip_cop );
    }
    for( vect_string::const_iterator i = node->top_for.begin(); i != node->top_for.end(); ++i ) {
      p_conv_op_t cop = get_op( *i );
      if( !cop->on_seen_top() ) { continue; } // wait till we've seen all tops to process an op
      add_bck_ops_op( cop );
      for( vect_string::const_iterator j = cop->bots.begin(); j != cop->bots.end(); ++j ) { add_bck_ops_rec( *j ); }
    }
  }
  void conv_pipe_t::add_bck_ops( void ) {
    assert( !has_bck_ops.v );
    topo_visit_setup();
    vect_string fwd_tops{ tops.begin(), tops.end() };
    for( vect_string::const_iterator i = fwd_tops.begin(); i != fwd_tops.end(); ++i ) {
      // when add_bck_ops==1, we assume that all tops should be produced by a SoftmaxWithLoss operation. that is, we
      // assume that the 'real' or raw outputs of the fwd net are already 'capped' with a combo
      // loss-function/fwd-top-gradient-producing node. we check that here:
      p_conv_node_t node = must_get_node( *i );
      assert_st( node->top_for.size() == 1 );
      if( !get_op(node->top_for[0])->is(SoftmaxWithLoss_coi) ) {
	rt_err( strprintf( "add_bck_ops: unhandled: top node %s not produced by SoftmaxWithLoss op", str(*i).c_str() ) );
      }
      add_bck_ops_rec( *i ); 
    }
    has_bck_ops.v = 1;
  }


  void conv_pipe_t::fwd_alloc_ndas( p_map_str_p_nda_float_t const & fwd, bool const & sinks_only ) {
    for( map_str_p_conv_node_t::const_iterator i = nodes->begin(); i != nodes->end(); ++i ) {
      p_conv_node_t const & node = i->second;
      dims_t node_dims = node->dims;
      node_dims.calc_strides(); // for now, assume no padding
      if( node->top_for.empty() ) { 
	//printf( "must_find(*fwd,node->name)->dims=%s node_dims=%s\n", str(must_find(*fwd,node->name)->dims).c_str(), str(node_dims).c_str() );
	assert_st( must_find( *fwd, node->name )->dims == node_dims ); 
      }
      else if( (!sinks_only) || node->bot_for.empty() ) {
	must_insert( *fwd, node->name, make_shared<nda_float_t>( node_dims ) );
      }
    }
  }

  // assumes the single input blob is the 'data' blob (and there shouldn't be others)
  p_nda_float_t conv_pipe_t::run_one_blob_in_one_blob_out( p_nda_float_t const & in, p_has_conv_fwd_t const & conv_fwd ) {
    p_map_str_p_nda_float_t fwd = make_shared<map_str_p_nda_float_t>( *op_params );
    if( data_img_node_names.size() != 1 ) { rt_err( "run_one_blob_in_one_blob_out only supports exactly one image input" ); }
    (*fwd)[data_img_node_names[0]] = in;
    // FIXME: hack for now to set labels (if needed) to something arbirtraty
    if( data_label_node_names.size() ) {
      string const & lnn = data_label_node_names[0];
      assert_st( data_label_node_names.size() == data_img_node_names.size() ); // currently true by construction
      conv_io_t const & label_cio = must_get_node( lnn )->cio;
      p_nda_float_t label( new nda_float_t( must_get_node( lnn )->dims ) );
      uint32_t lix = 0;
      for( dims_iter_t di( label->dims ) ; ; ) { label->at(di.di) = lix % label_cio.max_val; ++lix; if( !di.next() ) { break; } } 
      (*fwd)[lnn] = label;
    }
    vect_string missing_inputs;
    for( set_string::const_iterator i = bots.begin(); i != bots.end(); ++i ) { if( !has(*fwd,*i) ) { missing_inputs.push_back( *i ); } }
    if( !missing_inputs.empty() ) { rt_err( "run_one_blob_in_one_blob_out: missing_inputs (not images/labesl from data layers? internal error?): " + 
					    str(missing_inputs) ); } 
    assert( conv_fwd );
    conv_fwd->run_fwd( vect_string{bots.begin(),bots.end()}, fwd, {get_single_top_node()->name} );
    return must_find( *fwd, get_single_top_node()->name );
  }

  // FIXME: we *alter* the dims (especially the names) of blobs here. does that makes sense? generally, the blobs are
  // unused after this by the caller *and* the modifications are correct/sensible. but maybe the caller should have done
  // these modifications, not us?
  void conv_pipe_t::add_layer_blobs( string const & rln, p_vect_p_nda_float_t const & blobs ) {
    if( blobs->empty() ) { return; } // if no blobs to copy, we don't require a matching op exist in the pipe
    p_conv_op_t const & cop = get_op( rln );
    vect_string bsb_names;
    if( cop->is( Convolution_coi ) ) { 
      assert( blobs->size() == 2 );
      bsb_names.push_back( cop->tag + "_filts" ); 
      bsb_names.push_back( cop->tag + "_biases" ); 
    }
    else { for( uint32_t i = 0; i != blobs->size(); ++i ) { bsb_names.push_back( cop->tag + "_" + str(i) ); } }
    assert_st( bsb_names.size() == blobs->size() );
    for( uint32_t i = 0; i != bsb_names.size(); ++i ) { 
      assert_st( op_params->insert( std::make_pair( bsb_names[i], blobs->at(i) ) ).second );
    }
    must_insert( *layer_blobs, rln, blobs );
  }

  struct conv_ana_t : virtual public nesi, public has_main_t // NESI(help="analysize pipeline of convolutions wrt sizes at each layer, strides, padding, and per-layer-input-sizes (aka support sizes). ",bases=["has_main_t"], type_id="conv_ana")
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support
    p_vect_conv_op_t convs; //NESI(default="()",help="set of conv-ish ops")
    filename_t out_fn; //NESI(default="%(boda_output_dir)/out.txt",help="text output filename")
    // filename_t convs_fn; NESI(help="input: filename for list of convs",req=1)
    p_uint32_t in_sz; //NESI(help="calculate sizes at all layers for the given input size and dump pipe")
    uint32_t in_chans; //NESI(default=3,help="number of input chans (used only to properly print number of input chans)")
    p_uint32_t out_sz; //NESI(help="calculate sizes at all layers for the given output size and dump pipe")

    uint32_t print_ops; //NESI(default=0,help="if non-zero, print ops. note: requires in_sz to be set.")

    uint32_t ignore_padding_for_support; //NESI(default=1,help="if 1, ignore any padding specified when calculating the support_size for a single pel for each layer")
#if 0
    // FIXME-MAYBE: we lost the ability to handle ignore-padding for sz during the sz->dims refactoring. we could
    // perhaps add it back by dynamically removing padding from the input net and/or conv_pipe before doing the various
    // operations. this might not be quite the same as the old functionality, but maybe that's okay. or maybe we can
    // ignore this forever.
    uint32_t ignore_padding_for_sz; //xNESI(default=0,help="if 1, ignore any padding specified when calculating the sizes at each layer for the in_sz or out_sz options")
#endif
    
    virtual void main( nesi_init_arg_t * nia ) { 
      // convert 'legacy' conv_ana linear pipe input to general net
      p_conv_pipe_t conv_pipe( new conv_pipe_t ); 
      string cur_node_name = "input";

      p_conv_node_t const data_img_node = conv_pipe->get_or_make_node(cur_node_name, 0, 0 );
      assert( !data_img_node->csi.valid() );
      data_img_node->csi.support_sz = u32_pt_t(1,1);
      data_img_node->csi.support_stride = u32_pt_t(1,1);
      data_img_node->dims = dims_t( vect_uint32_t{ 1, in_chans, in_sz ? *in_sz : 1, in_sz ? *in_sz : 1 }, vect_string{ "img", "chan", "y", "x" }, 1 );

      for( vect_conv_op_t::const_iterator i = convs->begin(); i != convs->end(); ++i ) {
	p_conv_op_t cop( new conv_op_t( *i ) );
	assert_st( cop->tops.empty() && cop->bots.empty() );
	cop->bots.push_back( cur_node_name );
	cur_node_name = cop->tag + "_out";
	cop->tops.push_back( cur_node_name );
	conv_pipe->add_conv( cop );
      }

      p_ofstream out = ofs_open( out_fn.exp );
      //(*out) << convs << "\n";
      conv_pipe->calc_support_info( ignore_padding_for_support );
      conv_pipe->calc_dims();
      conv_pipe->dump_pipe( *out ); 
      if( in_sz ) { 
	(*out) << ">> calculating network sizes forward given an in_sz of " << *in_sz << "\n";
	conv_pipe->dump_ios( *out ); 
	if( print_ops ) { conv_pipe->dump_ops( *out, 0 ); }
      }

      if( out_sz ) { 
	(*out) << ">> calculating network sizes backward given an out_sz of " << *out_sz << "\n";
	conv_pipe->calc_sizes_back( u32_pt_t( *out_sz, *out_sz ), 0 ); // ignore_padding_for_sz ); 
	conv_pipe->dump_ios( *out ); 
	conv_pipe->clear_sizes();
      }
    }
  };

  p_net_param_t conv_pipe_t::as_net_param( void ) const { assert( orig_net_param ); return orig_net_param; }

#include"gen/conv_util.H.nesi_gen.cc"
#include"gen/conv_util.cc.nesi_gen.cc"

};
