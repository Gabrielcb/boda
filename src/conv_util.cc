// Copyright (c) 2013-2014, Matthew W. Moskewicz <moskewcz@alumni.princeton.edu>; part of Boda framework; see LICENSE
#include"boda_tu_base.H"
#include"conv_util.H"

#include"timers.H"
#include"str_util.H"
#include"has_main.H"
#include"io_util.H"
#include"nesi.H"
#include"caffepb.H"

namespace boda 
{

  u32_pt_t conv_op_t::in_sz_to_out_sz( u32_pt_t const & in_sz, bool const ignore_padding ) const { 
    if( kern_sz.is_zeros() ) { // handle non-conv cases
      assert( type != Convolution_str ); 
      if( (type == Pooling_str) || (type == InnerProduct_str) ) { return u32_pt_t{1,1}; } // global pooling / inner product special cases
      return in_sz; // otherwise, assume no effect on spatial dims (e.g. relu, lrn)
    }
    u32_pt_t const pad_in_sz = in_sz+(ignore_padding?u32_pt_t():in_pad.bnds_sum());
    if( !pad_in_sz.both_dims_ge(kern_sz) ) { return u32_pt_t(); } // padded input too small to create any output
    if( type == Convolution_str ) { return (pad_in_sz-kern_sz)/stride + u32_pt_t(1,1); }
    else if( type == Pooling_str ) { return ceil_div( pad_in_sz-kern_sz,stride ) + u32_pt_t(1,1); }
    else { rt_err("unknown layer type"); }
  }
  u32_pt_t conv_op_t::out_sz_to_in_sz( u32_pt_t const & out_sz, bool const ignore_padding ) const { 
    if( kern_sz.is_zeros() ) { // handle non-conv cases
      assert( type != Convolution_str );
      if( (type == Pooling_str) || (type == InnerProduct_str) ) { // inner product and global pooling special cases
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

  void conv_pipe_t::finalize( void ) {
    assert_st( !finalized ); // could relax
    assert_st( tops.empty() );
    assert_st( bots.empty() );
    for( map_str_p_conv_node_t::const_iterator i = nodes->begin(); i != nodes->end(); ++i ) {
      p_conv_node_t const & in = i->second;
      if( in->top_for.empty() ) { bots.push_back( in->name ); }
      if( in->bot_for.empty() ) { tops.push_back( in->name ); }
    }
    finalized = 1;
  }

  // this returns the single unique input node of the net or throws an error
  p_conv_node_t conv_pipe_t::get_single_bot_node( void ) const {
    p_conv_node_t ret;
    for( map_str_p_conv_node_t::const_iterator i = nodes->begin(); i != nodes->end(); ++i ) {
      p_conv_node_t const & in = i->second;
      if( in->top_for.empty() ) { 
	if( ret ) { rt_err( strprintf( "multiple source/input nodes found in net; can't process. two examples:'%s','%s'", 
				       ret->name.c_str(), in->name.c_str() ) ); }
	ret = in;
      }
    }
    if( !ret ) { rt_err( "no source/input nodes found in net; can't process. perhaps this is an invalid circular net?" ); }
    return ret;
  }

  p_conv_node_t conv_pipe_t::get_single_top_node( void ) const {
    p_conv_node_t ret;
    for( map_str_p_conv_node_t::const_iterator i = nodes->begin(); i != nodes->end(); ++i ) {
      p_conv_node_t const & in = i->second;
      if( in->bot_for.empty() ) { 
	if( ret ) { rt_err( strprintf( "multiple sink/output nodes found in net; can't process. two examples:'%s','%s'", 
				       ret->name.c_str(), in->name.c_str() ) ); }
	ret = in;
      }
    }
    if( !ret ) { rt_err( "no sink/output nodes found in net; can't process. perhaps this is an invalid circular net?" ); }
    return ret;
  }
  
  p_conv_node_t conv_pipe_t::get_or_make_node( string const & name ) {
    p_conv_node_t & ret = (*nodes)[name];
    if( !ret ) { ret.reset( new conv_node_t{name} ); }
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
    assert_st( !finalized );
    bool did_ins = convs->insert( make_pair( conv->tag, conv ) ).second;
    if( !did_ins ) { rt_err( strprintf( "duplicate conv op '%s' seen; can't process net", conv->tag.c_str() ) ); }
    for( vect_string::const_iterator i = conv->tops.begin(); i != conv->tops.end(); ++i ) {
      get_or_make_node( *i )->top_for.push_back( conv->tag );
    }
    for( vect_string::const_iterator i = conv->bots.begin(); i != conv->bots.end(); ++i ) {
      get_or_make_node( *i )->bot_for.push_back( conv->tag );
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
  p_conv_op_t conv_pipe_t::maybe_get_single_reader( p_conv_node_t const & node ) const {
    if( node->bot_for.empty() ) { return p_conv_op_t(); }
    if( node->bot_for.size() != 1 ) { 
      printstr( "WARNING: unhandled multiple readers for node: " + node->name + "\n" ); 
      //rt_err( "unhandled multiple readers for node: " + node->name ); 
    }
    return get_op( node->bot_for[0] );
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


  void conv_pipe_t::zero_conv_ios( vect_conv_io_t & conv_ios ) {
    conv_ios.clear();
    conv_ios.resize( convs->size() + 1 );
    for( vect_conv_io_t::iterator i = conv_ios.begin(); i != conv_ios.end(); ++i ) {
      i->sz = u32_pt_t(); i->used_sz = u32_pt_t(); i->chans = 0;
    }
  }

  void conv_pipe_t::calc_support_forward_rec( p_conv_node_t const & node_in, bool const ignore_padding ) {
    conv_support_info_t const & csi_in = node_in->csi;
    // propogate support info forward from node to all ops that it feeds and thier outputs
    for( vect_string::const_iterator i = node_in->bot_for.begin(); i != node_in->bot_for.end(); ++i ) {
      p_conv_op_t const & cop = get_op( *i );
      if( !cop->on_seen_bot() ) { continue; } // wait till we've seen all bottoms
      assert_st( cop->has_one_top() );
      p_conv_node_t const & node_out = must_get_node(cop->tops[0]);
      conv_support_info_t & csi_out = node_out->csi;
      if( csi_out.valid() ) { rt_err( "unhandled: node with multiple writers:"+node_out->name ); }
      u32_pt_t const in_sz_1x1 = cop->out_sz_to_in_sz( u32_pt_t(1,1), ignore_padding ); // == cop.kern_sz (if ign_pad)
      if( in_sz_1x1.is_zeros() || csi_in.support_sz.is_zeros() )  { // special values that means use all input
	csi_out.support_sz = u32_pt_t{};
      } else {
	assert_st( in_sz_1x1.both_dims_non_zero() );
	csi_out.support_sz = csi_in.support_sz + ( in_sz_1x1 - u32_pt_t(1,1) )*csi_in.support_stride;
      }
      assert_st( cop->stride.both_dims_non_zero() );
      csi_out.support_stride = csi_in.support_stride*cop->stride;
      // traverse backward to root to calculate eff_tot_pad
      for( p_conv_op_t cop_back = cop; cop_back; cop_back = maybe_get_single_parent(cop_back) ) {
	csi_out.eff_tot_pad = cop_back->in_pad + csi_out.eff_tot_pad.scale_dims( cop_back->stride );	
      }
      calc_support_forward_rec( node_out, ignore_padding ); // depth-first recursive processing for any outputs
    }
  }

  // generally more sensible to with ignore_padding_for_support = 1 (but possibly interesting if = 0 too)
  void conv_pipe_t::calc_support_info( bool const ignore_padding ) {
    // initialize support info for single root input
    p_conv_node_t const & node = get_single_bot_node();
    conv_support_info_t & csi = node->csi;
    assert( !csi.valid() );
    csi.support_sz = u32_pt_t(1,1);
    csi.support_stride = u32_pt_t(1,1);
    topo_visit_setup();
    calc_support_forward_rec( node, ignore_padding ); // calculate support
  }

  
  void conv_pipe_t::clear_sizes( void ) {
    for( map_str_p_conv_node_t::iterator i = nodes->begin(); i != nodes->end(); ++i ) { i->second->cio = conv_io_t(); }
  }
  void conv_pipe_t::topo_visit_setup( void ) {
    for( map_str_p_conv_op_t::iterator i = convs->begin(); i != convs->end(); ++i ) { i->second->bots_seen = 0; }
  }

  void conv_pipe_t::calc_sizes_forward_rec( p_conv_node_t const & node_in, bool const ignore_padding ) {
    // propogate support info forward from node to all ops that it feeds and thier outputs
    for( vect_string::const_iterator i = node_in->bot_for.begin(); i != node_in->bot_for.end(); ++i ) {
      p_conv_op_t const & cop = get_op( *i );
      if( !cop->on_seen_bot() ) { continue; } // wait till we've seen all bottoms
      assert_st( cop->has_one_top() );
      p_conv_node_t const & node_out = must_get_node(cop->tops[0]);
      conv_io_t & cio_out = node_out->cio;
      if( cio_out.valid() ) { rt_err( "node size calculation is not supported for reconvegent networks at node:"+node_out->name ); }

      // FIXME: move to own func
      uint32_t const & out_chans = cop->out_chans; 
      if( (cop->bots.size() != 1) && (cop->type != "Concat") ) { 
	rt_err( "unhandled multi-input operation: "+cop->tag+" of type " + cop->type+" " ); }
      for( vect_string::const_iterator j = cop->bots.begin(); j != cop->bots.end(); ++j ) {
	conv_io_t & cio_in = must_get_node(*j)->cio; // note: non-const since cio_in.used_sz is updated
	if( j == cop->bots.begin() ) { // first input 
	  cio_out.sz = cop->in_sz_to_out_sz( cio_in.sz, ignore_padding );
	  if( cio_out.sz.both_dims_non_zero() ) { 
	    cio_in.used_sz.max_eq( cop->out_sz_to_in_sz( cio_out.sz, ignore_padding ) );
	  } // else if there's no output, we used no input (used_sz left at zero)
	  // reset or propogate num_chans
	  cio_out.chans = out_chans ? out_chans : cio_in.chans;
	} else { // handle multiple inputs for concat layer (only!)
	  assert( cop->type == "Concat" );
	  assert( !out_chans );
	  // x/y dims must agree across all inputs
	  u32_pt_t const out_sz = cop->in_sz_to_out_sz( cio_in.sz, ignore_padding );
	  assert_st( out_sz == cio_out.sz );
	  // sum chans across all inputs
	  cio_out.chans += cio_in.chans;
	}
      }
      calc_sizes_forward_rec( node_out, ignore_padding ); // depth-first recursive processing for any outputs
    }
  }
  void conv_pipe_t::calc_sizes_forward( u32_pt_t const & in_sz, uint32_t const & in_chans, bool const ignore_padding ) {
    // initialize support info for single root input
    p_conv_node_t const & node = get_single_bot_node();
    conv_io_t & cio = node->cio;
    assert( !cio.valid() );
    cio.sz = in_sz;
    cio.chans = in_chans;
    topo_visit_setup();
    calc_sizes_forward_rec( node, ignore_padding ); // calculate support
  }

  // note: recursively sturctured, but only works for chains currently. it's unclear what the
  // extention to non-chains would be exactly, but it would seem to depend on handling some
  // particular type of conv_op with >1 input.
  void conv_pipe_t::calc_sizes_back_rec( p_conv_node_t const & node_out, bool const ignore_padding ) {
    conv_io_t const & cio_out = node_out->cio;
    p_conv_op_t cop = maybe_get_single_writer( node_out );
    if( !cop ) { return; } // reached source, done
    assert_st( cop->has_one_top_one_bot() );
    p_conv_node_t const & node_in = must_get_node(cop->bots[0]);
    conv_io_t & cio_in = node_in->cio;
    if( cio_in.valid() ) { rt_err( "internal error: cio_in.valid() in calc_sizes_back_rec() at node:"+node_out->name ); }
    if( !cio_out.sz.both_dims_non_zero() ) {
      rt_err( strprintf( "calc_sizes_back(): unhandled/questionable case: pipeline stage %s output is zero-area.",
			 cop->tag.c_str() ) );
    }
    cio_in.sz = cop->out_sz_to_in_sz( cio_out.sz, ignore_padding );
    cio_in.used_sz = cio_in.sz; // by semantics of out_sz_to_in_sz (but checked below)
    cio_in.chans = 1; // FIXME: just to mark as valid
    assert_st( cio_out.sz == cop->in_sz_to_out_sz( cio_in.sz, ignore_padding ) );
    calc_sizes_back_rec( node_in, ignore_padding ); // depth-first recursive processing for the input
  }

  void conv_pipe_t::calc_sizes_back( u32_pt_t const & out_sz, bool const ignore_padding ) {
    // initialize support info for single output
    p_conv_node_t const & node = get_single_top_node();
    conv_io_t & cio = node->cio;
    assert( !cio.valid() );
    cio.sz = out_sz;
    cio.chans = 1; // FIMXE: allow specification? meaningful?
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

      assert_st( cop->has_one_top() );
      dump_pipe_rec( out, cop->tops[0] );
    }
  }

  void conv_pipe_t::dump_pipe( std::ostream & out ) {
    assert_st( finalized );
    out << strprintf( "== BEGIN CONV PIPE ==\n" );
    topo_visit_setup();
    for( vect_string::const_iterator i = bots.begin(); i != bots.end(); ++i ) { dump_pipe_rec( out, *i ); }
    out << strprintf( "== END CONV PIPE ==\n" );
  }

  void conv_pipe_t::dump_ios_rec( std::ostream & out, string const & node_name ) {
    p_conv_node_t node = must_get_node( node_name );
    if( node->bot_for.size() > 1 ) { 
      out << strprintf("(-->" ); 
      for( vect_string::const_iterator i = node->bot_for.begin(); i != node->bot_for.end(); ++i ) { out << " " << *i; }
      out << strprintf(")");
    }
    conv_io_t const & cio = node->cio;
    out << strprintf( "sz=%s -> ", str(cio.sz).c_str() );
    string size_err;
    if( cio.sz != cio.used_sz ) { 
      if( (cio.used_sz.d[0] > cio.sz.d[0]) || (cio.used_sz.d[1] > cio.sz.d[1]) ) { size_err += "IMPLICIT PAD; "; }
      if( (cio.used_sz.d[0] < cio.sz.d[0]) || (cio.used_sz.d[1] < cio.sz.d[1]) ) { size_err += "DATA DISCARDED; "; }
      out << strprintf( "[%sused_sz=%s] -> ", size_err.c_str(), str(cio.used_sz).c_str() );
    }
    for( vect_string::const_iterator i = node->bot_for.begin(); i != node->bot_for.end(); ++i ) {
      p_conv_op_t const & cop = get_op( *i );
      if( !cop->on_seen_bot() ) { continue; } // wait till we've seen all bottoms
      out << cop->tag << " -> ";
      assert_st( cop->has_one_top() );
      dump_ios_rec( out, cop->tops[0] );
    }
  }
  void conv_pipe_t::dump_ios( std::ostream & out ) {
    assert_st( finalized );
    out << "CONV_IOS: ";
    topo_visit_setup();
    for( vect_string::const_iterator i = bots.begin(); i != bots.end(); ++i ) { dump_ios_rec( out, *i ); }
    out << "\n";
  }

  void print_blob_decl( string const & bn, p_conv_node_t const & node ) {
    string isss;
    if( node->top_for.empty() ) { isss += " SOURCE"; }
    if( node->bot_for.empty() ) { isss += " SINK"; }
    conv_io_t & cio = node->cio;
    printf( "%s = NDA(num_img,%s,%s,%s) #%s num,chan,y,x\n", 
	    bn.c_str(), str(cio.chans).c_str(), str(cio.sz.d[1]).c_str(), str(cio.sz.d[0]).c_str(), isss.c_str() );
  }

  void print_op_decl( conv_pipe_t const * const pipe, p_conv_op_t const & cop ) {
    string extra_params;
    if( cop->type == "Convolution" || cop->type == "InnerProduct" ) {
      char const * const tag = cop->tag.c_str();
      assert_st( cop->bots.size() == 1 );
      conv_io_t & cio = pipe->must_get_node( cop->bots[0] )->cio;
      u32_pt_t kern_sz = cop->kern_sz;
      if( kern_sz.is_zeros() ) { kern_sz = cio.sz; }
      printf( "%s_filts = NDA(1,%s,%s,%s) # SOURCE 1,chan,y,x\n", 
	      tag, str(cop->out_chans).c_str(), str(kern_sz.d[1]).c_str(), str(kern_sz.d[0]).c_str() );
      printf( "%s_biases = NDA(1,%s,1,1) # SOURCE 1,chan,1,1\n", 
	      tag, str(cop->out_chans).c_str() );
      extra_params = strprintf( ",%s_filts,%s_biases", tag, tag );
    }

    printf( "%s(bots=%s,tops=%s%s,in_pad=%s,stride=%s) # %s\n", 
	    cop->type.c_str(), str(cop->bots).c_str(), str(cop->tops).c_str(),
	    extra_params.c_str(),
	    str(cop->in_pad).c_str(), str(cop->stride).c_str(),
	    cop->tag.c_str() );
  }

  void conv_pipe_t::dump_ops_rec( std::ostream & out, string const & node_name ) {
    p_conv_node_t node = must_get_node( node_name );
    print_blob_decl( node_name, node );
    for( vect_string::const_iterator i = node->bot_for.begin(); i != node->bot_for.end(); ++i ) {
      p_conv_op_t const & cop = get_op( *i );
      if( !cop->on_seen_bot() ) { continue; } // wait till we've seen all bottoms
      print_op_decl( this, cop );
      assert_st( cop->has_one_top() );
      dump_ops_rec( out, cop->tops[0] );
    }
  }

  void conv_pipe_t::dump_ops( std::ostream & out ) {
    assert_st( finalized );
    topo_visit_setup();
    for( vect_string::const_iterator i = bots.begin(); i != bots.end(); ++i ) { dump_ops_rec( out, *i ); }
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
    uint32_t ignore_padding_for_sz; //NESI(default=0,help="if 1, ignore any padding specified when calculating the sizes at each layer for the in_sz or out_sz options")
    uint32_t print_ops; //NESI(default=0,help="if non-zero, print ops. note: requires in_sz to be set.")
    uint32_t ignore_padding_for_support; //NESI(default=1,help="if 1, ignore any padding specified when calculating the support_size for a single pel for each layer")
    
    virtual void main( nesi_init_arg_t * nia ) { 
      // convert 'legacy' conv_ana linear pipe input to general net
      p_conv_pipe_t conv_pipe( new conv_pipe_t ); 
      string cur_node_name = "input";
      for( vect_conv_op_t::const_iterator i = convs->begin(); i != convs->end(); ++i ) {
	p_conv_op_t cop( new conv_op_t( *i ) );
	assert_st( cop->tops.empty() && cop->bots.empty() );
	cop->bots.push_back( cur_node_name );
	cur_node_name = cop->tag + "_out";
	cop->tops.push_back( cur_node_name );
	conv_pipe->add_conv( cop );
      }
      conv_pipe->finalize();

      p_ofstream out = ofs_open( out_fn.exp );
      //(*out) << convs << "\n";
      conv_pipe->calc_support_info( ignore_padding_for_support );
      conv_pipe->dump_pipe( *out ); 
      if( out_sz ) { 
	(*out) << ">> calculating network sizes backward given an out_sz of " << *out_sz << "\n";
	conv_pipe->calc_sizes_back( u32_pt_t( *out_sz, *out_sz ), ignore_padding_for_sz ); 
	conv_pipe->dump_ios( *out ); 
	conv_pipe->clear_sizes();
      }
      p_vect_conv_io_t conv_ios;
      if( in_sz ) { 
	(*out) << ">> calculating network sizes forward given an in_sz of " << *in_sz << "\n";
	conv_pipe->calc_sizes_forward( u32_pt_t( *in_sz, *in_sz ), in_chans, ignore_padding_for_sz ); 
	conv_pipe->dump_ios( *out ); 
	if( print_ops ) { conv_pipe->dump_ops( *out ); }
	conv_pipe->clear_sizes();	
      }
    }
  };

#include"gen/conv_util.H.nesi_gen.cc"
#include"gen/conv_util.cc.nesi_gen.cc"

};
