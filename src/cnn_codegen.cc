// Copyright (c) 2015, Matthew W. Moskewicz <moskewcz@alumni.princeton.edu>; part of Boda framework; see LICENSE
#include"boda_tu_base.H"

#include"rtc_func_gen.H"
#include"geom_prim.H"

namespace boda 
{
  struct cnn_custom_codegen_t : public custom_codegen_t {

    virtual void gen_op( rtc_call_gen_t * rcg, string const & op_name ) {
      // *** custom codegen hooks ***
      if( op_name == "conv" ) { gen_op_conv(rcg); } 
      else if( op_name == "ipconv" ) { gen_op_ipconv(rcg); } 
      else if( op_name == "k1conv" ) { gen_op_k1conv(rcg); } 
      else if( op_name == "tconv" ) { gen_op_tconv(rcg); } 
      else if( op_name == "bwai" ) { gen_op_bwai(rcg); } 
      else if( op_name == "bconv" ) { gen_op_bconv(rcg); } 
      else if( op_name == "bconv_fb" ) { gen_op_bconv_fb(rcg); } 
      else if( op_name == "reduce" ) { gen_op_reduce(rcg); } 
    }

    void gen_op_reduce( rtc_call_gen_t * rcg ) {
      for( uint32_t i = 0; i != rcg->flat_arg_decls.size() - 1; ++i ) { 
	rcg->line( "ins_ops", "v += ins_"+str(i)+"[GLOB_ID_1D];" ); 
      }
    }

    string add_bias_then_maybe_relu( rtc_call_gen_t * rcg, dims_t const & work, uint32_t const & tx, uint32_t const ty ) { 
      string const ve = strprintf( "(out_tile[%s] + filts_strip[%s])", str((ty*work.dsz("out_chan")+tx)).c_str(), str(tx).c_str() );
      return rcg->get_u32("conv_has_relu") ? ( "max(0.0f,"+ve+")" ) : ve;
    }    

    void gen_op_bconv( rtc_call_gen_t * rcg ) {
      dims_t const & work = rcg->get_arg_dims_by_name( "work" );
      uint32_t const in_smem_sz = work.dsz("pels_tile")*work.dsz("pels");
      rcg->set( "in_smem_sz", str(in_smem_sz) );
      uint32_t const in_smem_load_iter = u32_ceil_div( in_smem_sz, rcg->tpb );
      rcg->set( "in_smem_load_iter", str(in_smem_load_iter) );    

      uint32_t const filts_smem_sz = work.dsz("out_ix_tile")*work.dsz("out_ix");
      rcg->set( "filts_smem_sz", str(filts_smem_sz) );
      uint32_t const filts_smem_load_iter = u32_ceil_div( filts_smem_sz, rcg->tpb );
      rcg->set( "filts_smem_load_iter", str(filts_smem_load_iter) );    

      for( uint32_t tx = 0; tx != work.dsz( "out_ix" ); ++tx ) {
	rcg->line( "loads", strprintf( "filts_strip[%s] = filts_smem[%%(LOC_ID_1D_out_ix_tile)*%%(work_out_ix_dim)+%s];",
					 str(tx).c_str(), str(tx).c_str() ) );
      }
      for( uint32_t ty = 0; ty != work.dsz( "pels" ); ++ty ) { // note: could merge with above loop, but we want to use ty for consistency
	rcg->line( "loads", strprintf( "in_strip[%s] = in_smem[%%(LOC_ID_1D_pels_tile)*%%(work_pels_dim)+%s];",
					 str(ty).c_str(), str(ty).c_str() ) );
      }

      rcg->line( "outs_to_filts_strip", "switch(work_pel) { " );
      for( uint32_t ty = 0; ty != work.dsz( "pels" ); ++ty ) {
	rcg->line( "outs_to_filts_strip", "case "+str(ty)+":" );
	for( uint32_t tx = 0; tx != work.dsz( "out_ix" ); ++tx ) {
	  uint32_t const rix = ty*work.dsz("out_ix")+tx;
	  rcg->line( "fmas", strprintf( "out_tile[%s] += filts_strip[%s]*in_strip[%s];", 
					  str(rix).c_str(), str(tx).c_str(), str(ty).c_str() ) );
	  rcg->line( "outs_to_filts_strip", strprintf( "filts_strip[%s] = out_tile[%s];", 
					    str(tx).c_str(), str(rix).c_str() ) );	  
	}
	rcg->line( "outs_to_filts_strip", "break;" );
      }
      rcg->line( "outs_to_filts_strip", "} " );

      string store_expr = R"foo(
  igl_y = (%(pel_ix_y)-%(bck_in_pad_y_dim))*%(stride_y_dim)+%(out_ix_sy)-%(in_pad_y_dim)+%(bck_pad_in_off_y_dim);
  igl_x = (%(pel_ix_x)-%(bck_in_pad_x_dim))*%(stride_x_dim)+%(out_ix_sx)-%(in_pad_x_dim)+%(bck_pad_in_off_x_dim);
  if( igl_x >= 0 && igl_y >= 0 && igl_y < %(in_grad_loss_y_dim) && igl_x < %(in_grad_loss_x_dim) &&
      %(out_ix_in_chan) < %(in_grad_loss_chan_dim) && %(pel_ix_img) < %(in_grad_loss_img_dim) ) {
    in_grad_loss[ %(pel_ix_img)*%(in_grad_loss_img_sz) + %(out_ix_in_chan)*%(in_grad_loss_chan_sz) + 
		  igl_y*%(in_grad_loss_y_sz) + igl_x*%(in_grad_loss_x_sz)] = filts_strip[)foo";
      for( uint32_t tx = 0; tx != work.dsz( "out_ix" ); ++tx ) {
	rcg->line( "stores", store_expr + strprintf( "%s];\n};", str(tx).c_str() ) );
	rcg->line( "stores", "++out_ix;" );
      }
    }

    void gen_op_bconv_fb( rtc_call_gen_t * rcg ) {
      dims_t const & work = rcg->get_arg_dims_by_name( "work_fb" );
      uint32_t const in_smem_sz = work.dsz("pels_tile")*work.dsz("pels");
      rcg->set( "in_smem_sz", str(in_smem_sz) );
      uint32_t const in_smem_load_iter = u32_ceil_div( in_smem_sz, rcg->tpb );
      rcg->set( "in_smem_load_iter", str(in_smem_load_iter) );    

      uint32_t const filts_smem_sz = work.dsz("out_ix_tile")*work.dsz("out_ix");
      rcg->set( "filts_smem_sz", str(filts_smem_sz) );
      uint32_t const filts_smem_load_iter = u32_ceil_div( filts_smem_sz, rcg->tpb );
      rcg->set( "filts_smem_load_iter", str(filts_smem_load_iter) );    

      for( uint32_t tx = 0; tx != work.dsz( "out_ix" ); ++tx ) {
	rcg->line( "loads", strprintf( "filts_strip[%s] = filts_smem[%%(LOC_ID_1D_out_ix_tile)*%%(work_fb_out_ix_dim)+%s];",
					 str(tx).c_str(), str(tx).c_str() ) );
      }
      for( uint32_t ty = 0; ty != work.dsz( "pels" ); ++ty ) { // note: could merge with above loop, but we want to use ty for consistency
	rcg->line( "loads", strprintf( "in_strip[%s] = in_smem[%%(LOC_ID_1D_pels_tile)*%%(work_fb_pels_dim)+%s];",
					 str(ty).c_str(), str(ty).c_str() ) );
      }

      rcg->line( "outs_to_filts_strip", "switch(work_pel) { " );
      for( uint32_t ty = 0; ty != work.dsz( "pels" ); ++ty ) {
	rcg->line( "outs_to_filts_strip", "case "+str(ty)+":" );
	for( uint32_t tx = 0; tx != work.dsz( "out_ix" ); ++tx ) {
	  uint32_t const rix = ty*work.dsz("out_ix")+tx;
	  rcg->line( "fmas", strprintf( "out_tile[%s] += filts_strip[%s]*in_strip[%s];", 
					  str(rix).c_str(), str(tx).c_str(), str(ty).c_str() ) );
	  rcg->line( "outs_to_filts_strip", strprintf( "filts_strip[%s] = out_tile[%s];", 
					    str(tx).c_str(), str(rix).c_str() ) );	  
	}
	rcg->line( "outs_to_filts_strip", "break;" );
      }
      rcg->line( "outs_to_filts_strip", "} " );

      string store_expr = R"foo(
  if( %(pel_ix_in_chan) < %(filts_grad_loss_in_chan_dim) && %(out_ix_out_chan) < %(filts_grad_loss_out_chan_dim) ) {
    filts_grad_loss[ %(out_ix_out_chan)*%(filts_grad_loss_out_chan_sz) + %(pel_ix_in_chan)*%(filts_grad_loss_in_chan_sz) + 
		  %(pel_ix_y)*%(filts_grad_loss_y_sz) + %(pel_ix_x)*%(filts_grad_loss_x_sz)] = filts_strip[)foo";
      for( uint32_t tx = 0; tx != work.dsz( "out_ix" ); ++tx ) {
	rcg->line( "stores", store_expr + strprintf( "%s];\n};", str(tx).c_str() ) );
	rcg->line( "stores", "++out_ix;" );
      }
    }
    
    void gen_filts_smem_loads( rtc_call_gen_t * rcg, uint32_t const filts_smem_sz ) { // note: filts_smem_sz must == tvv %(filts_smem_sz)
      uint32_t const out_chan_smem_load_iter = u32_ceil_div( filts_smem_sz, rcg->tpb );    
      for( uint32_t i = 0; i != out_chan_smem_load_iter; ++i ) {
	string const ixe = "(LOC_ID_1D + %(tpb) * "+str(i)+")";
	string eif;
	if( (i+1)*rcg->tpb > filts_smem_sz ) { 
	  rcg->line( "filts_smem_loads", "if( "+ixe+" < %(filts_smem_sz) ) {" );eif = "}";}
	// note: load is (always) contiguous
	rcg->line( "filts_smem_loads", strprintf("filts_smem[%s] = filts[filts_off+(%%(tpb)*%s)];%s",ixe.c_str(),str(i).c_str(),eif.c_str()) );
      }
      // number of out chans per block; note: == work_out_chan_tile_dim*work_out_chan_dim
      uint32_t const filts_x_sz = rcg->get_arg_dims_by_name("filts").dstride("x"); 
      uint32_t const out_chan_bias_smem_load_iter = u32_ceil_div( filts_x_sz, rcg->tpb );
      rcg->set( "out_chan_bias_smem_load_iter", str(out_chan_bias_smem_load_iter) );

      rcg->line( "biases_smem_loads","int32_t ocix; int32_t const ocix_base = %(GRP_ID_1D_out_chan_blk)*%(filts_x_sz);" );
      for( uint32_t i = 0; i != out_chan_bias_smem_load_iter; ++i ) {
	string const ixe = "(LOC_ID_1D + %(tpb) * "+str(i)+")";
	string eif;
	rcg->line( "biases_smem_loads", strprintf( "ocix = ocix_base + (%s %%%% %%(work_out_chan_tile_dim))*%%(work_out_chan_dim) + ( %s / %%(work_out_chan_tile_dim) );", ixe.c_str(), ixe.c_str() ) );
	if( (i+1)*rcg->tpb > filts_x_sz ) { 
	  rcg->line( "biases_smem_loads", "if( "+ixe+" < %(filts_x_sz) ) {" );eif = "}";}
	// note: load is (always) contiguous
	rcg->line( "biases_smem_loads", strprintf("if( ocix < %%(biases_out_chan_dim) ) {filts_smem[%s] = biases[ocix];}%s",ixe.c_str(),eif.c_str()) );
      }

    }

    void gen_op_conv( rtc_call_gen_t * rcg ) {
      dims_t const & work = rcg->get_arg_dims_by_name( "work" );
      dims_t const & filts = rcg->get_arg_dims_by_name( "filts" );
      uint32_t const filts_smem_sz = filts.dstride("x");
      rcg->set( "filts_smem_sz", str(filts_smem_sz) );
      gen_filts_smem_loads( rcg, filts_smem_sz );

      uint32_t const pel_smem_load_iter = u32_ceil_div( (work.dsz( "pels" ) * work.dsz( "pels_tile" )), rcg->tpb );
      rcg->set( "pel_smem_load_iter", str(pel_smem_load_iter) );
      rcg->set( "out_chan_tile", 
		"(%(LOC_ID_1D_out_chan_tile)+%(GRP_ID_1D_out_chan_blk)*%(work_out_chan_tile_dim))");
      rcg->set( "pel_tile",
		"(%(LOC_ID_1D_pels_tile)+%(GRP_ID_1D_pels_blk)*%(work_pels_tile_dim))");
      rcg->set( "out_chan_ix","(%(out_chan_tile)*%(work_out_chan_dim))" );
      for( uint32_t i = 0; i != work.dsz( "pels" ); ++i ) {
	insert_nda_ix_exprs( rcg->str_vals, "pel_ix_" + str(i), must_find(rcg->all_ix_dims,"out_pel_ix"),
			     strprintf( "(%%(pel_tile)*%%(work_pels_dim)+%s)", str(i).c_str() ) );
      }
      string const get_in = strprintf( 
	"float v = 0;\n"
	"      int const smem_in_ix_y = %%(out_pel_ix_y)*%%(stride_y_dim)+%%(filts_ix_out_chan_elem_y) - %%(in_pad_y_dim);\n"
	"      int const smem_in_ix_x = %%(out_pel_ix_x)*%%(stride_x_dim)+%%(filts_ix_out_chan_elem_x) - %%(in_pad_x_dim);\n"
	"      if(smem_in_ix_y >= 0 && smem_in_ix_x >= 0 && \n"
	"          %%(out_pel_ix_img) < %%(in_img_dim) && \n"
	"         smem_in_ix_x < %%(in_x_dim) && smem_in_ix_y < %%(in_y_dim) ) {\n"
	"        v = in[%%(out_pel_ix_img)*%%(in_img_sz) +\n"
	"          %%(filts_ix_out_chan_elem_in_chan)*%%(in_chan_sz) +\n"
	"          smem_in_ix_y*%%(in_y_sz) +\n"
	"          smem_in_ix_x*%%(in_x_sz)];\n" 
	"      }"
				       );
      rcg->set( "get_in", get_in );
      for( uint32_t tx = 0; tx != work.dsz( "out_chan" ); ++tx ) {
	rcg->line( "loads", strprintf( "filts_strip[%s] = filts_smem[%%(LOC_ID_1D_out_chan_tile)+%s*%%(work_out_chan_tile_dim)];",
				       str(tx).c_str(), str(tx).c_str() ) );
      }
      for( uint32_t ty = 0; ty != work.dsz( "pels" ); ++ty ) { // note: could merge with above loop, but we want to use ty for consistency
	rcg->line( "loads", strprintf( "in_strip[%s] = in_smem[%%(LOC_ID_1D_pels_tile)*%%(work_pels_dim)+%s];",
					 str(ty).c_str(), str(ty).c_str() ) );
      }
      rcg->line( "stores", "int32_t tpix[%(work_pels_dim)];");
      rcg->line( "stores", "int32_t tcix[%(work_out_chan_dim)];");
      // FIXME: should somehow assert that both out_ix and pel_ix_N have the same dims here
      for( uint32_t ty = 0; ty != work.dsz( "pels" ); ++ty ) { 
	rcg->line( "stores", 
		   strprintf( "tpix[%s] = %%(pel_ix_%s_img)*%%(out_img_sz) + "
			      "( %%(pel_ix_%s_x_nomod) %%%% (%%(out_y_dim)*%%(out_x_dim)) ); // cache out pel ixs ", // note: y:x adj-dim opt.
				str(ty).c_str(), str(ty).c_str(), str(ty).c_str() ) );
      }
      for( uint32_t ty = 0; ty != work.dsz( "out_chan" ); ++ty ) { 
	rcg->line( "stores", strprintf( "  tcix[%s] = (%%(out_chan_ix)+%s)*%%(out_chan_sz); // cache out chan ixs",
					  str(ty).c_str(), str(ty).c_str() ) );
      }	
      for( uint32_t ty = 0; ty != work.dsz( "pels" ); ++ty ) {
	rcg->line( "stores", "if( %(pel_ix_"+str(ty)+"_x_nomod) >= %(pel_ix_0_dims_prod) ) { return; } "
		     "// this pel and the following are off-the-end pels, so don't store them." );
	for( uint32_t tx = 0; tx != work.dsz( "out_chan" ); ++tx ) {
	  rcg->line( "fmas", strprintf( "out_tile[%s] += filts_strip[%s]*in_strip[%s];", 
					  str((ty*work.dsz( "out_chan" )+tx)).c_str(), str(tx).c_str(), str(ty).c_str() ) );
	  rcg->line( "stores", strprintf( "if( tcix[%s] < (%%(out_chan_dim)*%%(out_chan_sz)) ) { out[ tpix[%s] + tcix[%s] ] = %s; }",
					    str(tx).c_str(), str(ty).c_str(), str(tx).c_str(), 
					  add_bias_then_maybe_relu(rcg,work,tx,ty).c_str() ) );
	}
      }
    }

    void gen_op_ipconv( rtc_call_gen_t * rcg ) {
      dims_t const & work = rcg->get_arg_dims_by_name( "work" );
      //dims_t const & filts = get_arg_dims_by_name( "filts" );
      uint32_t const filts_smem_sz = work.dsz("out_chan_tile")*work.dsz("out_chan")*work.dsz("fioc_tile");
      rcg->set( "filts_smem_sz", str(filts_smem_sz) );
      uint32_t const out_chan_smem_load_iter = u32_ceil_div( filts_smem_sz, rcg->tpb );    
      for( uint32_t i = 0; i != out_chan_smem_load_iter; ++i ) {
	string const ixe = "(LOC_ID_1D + %(tpb) * "+str(i)+")";
	string const filt_ix = "( LOC_ID_1D/%(work_fioc_tile_dim) + %(tpb)/%(work_fioc_tile_dim)* "+str(i)+")";
	string eif;
	// FIXME: can load garbage when ((out_chan_dim % filts_per_blk) != 0). pad output? add conditionals here? ignore?
	if( (i+1)*rcg->tpb > filts_smem_sz ) { 
	  rcg->line( "filts_smem_loads", "if( "+ixe+" < %(filts_smem_sz) ) {" );eif = "}";}
	rcg->line( "filts_smem_loads", strprintf("filts_smem[%s] = filts[filts_off+(%s*%%(filts_out_chan_sz))];%s",ixe.c_str(),filt_ix.c_str(),eif.c_str()) );
      }

      uint32_t const in_smem_sz = work.dsz("pels_tile")*work.dsz("pels")*work.dsz("fioc_tile");
      rcg->set( "in_smem_sz", str(in_smem_sz) );
      uint32_t const in_smem_load_iter = u32_ceil_div( in_smem_sz, rcg->tpb );    
      // currently, ipconv can only handle one output point per image, and assume the filt and in data-layouts are the
      // same (hence the name ipconv, for inner-product-conv).
      for( uint32_t i = 0; i != in_smem_load_iter; ++i ) {
	string const ixe = "(LOC_ID_1D + %(tpb) * "+str(i)+")";
	string const img_ix = "( LOC_ID_1D/%(work_fioc_tile_dim) + %(tpb)/%(work_fioc_tile_dim)* "+str(i)+")";
	string eif;
	// FIXME: can load garbage when ((in_img_dim % imgs_per_blk) != 0). pad input? add conditionals here? ignore?
	if( (i+1)*rcg->tpb > in_smem_sz ) { 
	  rcg->line( "in_smem_loads", "if( "+ixe+" < %(in_smem_sz) ) {" );eif = "}";}
	rcg->line( "in_smem_loads", strprintf("in_smem[%s] = in[in_off+(%s*%%(in_img_sz))];%s",ixe.c_str(),img_ix.c_str(),eif.c_str()) );
      }

      for( uint32_t tx = 0; tx != work.dsz( "out_chan" ); ++tx ) {
	rcg->line( "loads", strprintf( "filts_strip[%s] = filts_smem_off[%s*%%(work_fioc_tile_dim)];",
					 str(tx).c_str(), str(tx).c_str() ) );
      }
      for( uint32_t ty = 0; ty != work.dsz( "pels" ); ++ty ) { // note: could merge with above loop, but we want to use ty for consistency
	rcg->line( "loads", strprintf( "in_strip[%s] = in_smem_off[%s*%%(work_fioc_tile_dim)];",
					 str(ty).c_str(), str(ty).c_str() ) );
      }
      rcg->line( "outs_to_filts_strip", "if( (in_pel+work_pel) >= %(in_img_dim) ) { return; } "
		   "// this pel and the following are off-the-end pels, so don't store them." );
      rcg->line( "outs_to_filts_strip", "switch(work_pel) { " );
      for( uint32_t ty = 0; ty != work.dsz( "pels" ); ++ty ) {
	rcg->line( "outs_to_filts_strip", "case "+str(ty)+":" );
	for( uint32_t tx = 0; tx != work.dsz( "out_chan" ); ++tx ) {
	  rcg->line( "fmas", strprintf( "out_tile[%s] += filts_strip[%s]*in_strip[%s];", 
					  str((ty*work.dsz( "out_chan" )+tx)).c_str(), str(tx).c_str(), str(ty).c_str() ) );
	  rcg->line( "outs_to_filts_strip", strprintf( "filts_strip[%s] = out_tile[%s];", 
					    str(tx).c_str(), str((ty*work.dsz("out_chan")+tx)).c_str() ) );	  
	}
	rcg->line( "outs_to_filts_strip", "break;" );
      }
      rcg->line( "outs_to_filts_strip", "} " );

      for( uint32_t tx = 0; tx != work.dsz( "out_chan" ); ++tx ) {
	string ve = strprintf( "(filts_strip[%s] + biases[ocix+%s])", str(tx).c_str(), str(tx).c_str() );
	ve = rcg->get_u32("conv_has_relu") ? ( "max(0.0f,"+ve+")" ) : ve;
	for( uint32_t wb = work.dsz("fioc_tile") / 2; wb; wb /= 2 ) {
	  rcg->line( "stores", strprintf( "filts_strip[%s] += __shfl_down( filts_strip[%s], %s, %s );", 
					    str(tx).c_str(), str(tx).c_str(), str(wb).c_str(), 
					    str( work.dsz("fioc_tile") ).c_str() ) );
	}
	rcg->line( "stores", strprintf( "if( (%%(LOC_ID_1D_fioc_tile) == 0 ) && ((ocix + %s) < %%(out_chan_dim)) ) "
					  "{ out[out_off + %s*%%(out_chan_sz)] = %s; }", 
					  str(tx).c_str(), str(tx).c_str(), str(ve).c_str() ) );
      }
    }

    void gen_op_bwai( rtc_call_gen_t * rcg ) {
      dims_t const & work = rcg->get_arg_dims_by_name( "work" );
      uint32_t const a_sm_sz = work.dsz("Mb")*work.dsz("Mt")*work.dsz("Kb");
      rcg->set( "a_sm_sz", str(a_sm_sz) );
      uint32_t const a_sm_load_iter = u32_ceil_div( a_sm_sz, rcg->tpb );    
      for( uint32_t i = 0; i != a_sm_load_iter; ++i ) {
	string const ixe = "(LOC_ID_1D + %(tpb) * "+str(i)+")";
	string const filt_ix = "( LOC_ID_1D/%(work_Kb_dim) + %(tpb)/%(work_Kb_dim)* "+str(i)+")";
	string eif;
	if( (i+1)*rcg->tpb > a_sm_sz ) { 
	  rcg->line( "sm_loads", "if( "+ixe+" < %(a_sm_sz) ) {" );eif = "}";}
	rcg->line( "sm_loads", strprintf("a_sm[%s] = a[a_off+(%s*%%(a_M_sz))];%s",ixe.c_str(),filt_ix.c_str(),eif.c_str()) );
      }

      uint32_t const b_sm_sz = work.dsz("Mb")*work.dsz("Mt")*work.dsz("Kb");
      rcg->set( "b_sm_sz", str(b_sm_sz) );

    }

    void gen_op_k1conv( rtc_call_gen_t * rcg ) {
      assert_st( get_xy_dims( rcg->get_arg_dims_by_name( "in_pad" ) ).is_zeros() );
      assert_st( (get_xy_dims( rcg->get_arg_dims_by_name( "stride" ) ) == u32_pt_t{1,1}) );
      dims_t const & work = rcg->get_arg_dims_by_name( "work" );
      dims_t const & filts = rcg->get_arg_dims_by_name( "filts" );
      assert_st( filts.dsz("x") == 1 ); assert_st( filts.dsz("y") == 1 );
      dims_t const & in = rcg->get_arg_dims_by_name( "in" );
      dims_t const & out = rcg->get_arg_dims_by_name( "out" );
      // calculate needed smem sizes (and total kernel needed smem size)
      // note: filts and in smem are used concurrently, then just all of all_smem as an output buffer
      uint32_t const filts_smem_sz = filts.dstride("in_chan")*in.dsz("blk_iter_chan");
      rcg->set( "filts_smem_sz", str(filts_smem_sz) );
      uint32_t const out_smem_sz = work.dsz("pels_tile")*work.dsz("out_chan_tile")*work.dsz("pels"); // note: == oi->tpb*work.dsz("pels")
      rcg->set( "out_smem_sz", str(out_smem_sz) ); // note: unused, but assumed that all_smem_sz >= out_smem_sz
      uint32_t const all_smem_sz = std::max( out_smem_sz, filts_smem_sz+in.dstride("blk_iter") ); // note: %(in_blk_iter_sz) == in_smem_sz
      rcg->set( "all_smem_sz", str(all_smem_sz) );

      // generate smem loads
      gen_filts_smem_loads( rcg, filts_smem_sz );
      uint32_t const in_smem_load_iter = u32_ceil_div( in.dstride("blk_iter"), rcg->tpb );    
      for( uint32_t i = 0; i != in_smem_load_iter; ++i ) {
	string const ixe = "(LOC_ID_1D + %(tpb) * "+str(i)+")";
	string eif;
	if( (i+1)*rcg->tpb > in.dstride("blk_iter") ) { rcg->line( "smem_loads", "if( "+ixe+" < %(in_blk_iter_sz)) { ");eif = "}";}
	rcg->line( "smem_loads", strprintf("    in_smem[%s] = in[ blk_in_ix_base + (%%(tpb)*%s) ];%s\n",
					     ixe.c_str(),str(i).c_str(),eif.c_str()) );
      }
      rcg->set( "out_chan_tile", "(%(GRP_ID_1D_out_chan_blk)*%(work_out_chan_tile_dim)+%(LOC_ID_1D_out_chan_tile))");
      rcg->set( "out_chan_ix","(%(out_chan_tile)*%(work_out_chan_dim))" );

      // rcg->line( "stores", "  if( %(out_line_img) >= %(out_ix_img_dim) ) { return; } "; // not possible due to no-partial-imgs-per-block
      // FIXME: should somehow assert that both out_ix and pel_ix_N have the same dims here
      // FIXME: out_pel must be per-tpix (again)
      if( out.get_dim_by_name("blk") ) { // aka if(write_xposed) -- if this dim names exists in the output, we know to write in xposed format
	// padded # of in chans of next layer  == out.dsz("blk_iter")*out.dsz("blk_iter_chan")
	// padded # of out chans of this layer == work.dsz("out_chan_blk")*work.dsz("out_chan_tile")*work.dsz("out_chan")
	// if these are ==, we don't have to worry about bounds-checking our writes to out in the chan dim
	assert_st( work.dsz("out_chan_blk")*work.dsz("out_chan_tile")*work.dsz("out_chan") == out.dsz("blk_iter")*out.dsz("blk_iter_chan") );
	// padded # of in pels of next layer:  == out.dsz("blk")*out.dsz("blk_pel")
	// padded # of out pels of this layer: == work.dsz("pels_blk")*work.dsz("pels_tile")*work.dsz("pels")
	// if these are ==, we don't have to worry about bounds-checking our writes to out in the pel dim
	assert_st( work.dsz("pels_blk")*work.dsz("pels_tile")*work.dsz("pels") == out.dsz("blk")*out.dsz("blk_pel") );

	// we assume out_blk_pel_dim (== noi->thr_per_blk.d[0]*t_tile_sz) is divisible by t_tile_sz. but let's check it explicitly:
	// FIXME_WXP: i don't see where we assume this, and hence i dunno what t_tile_sz refers to below. poop. assert is removed for now:
	// assert_st( (out.dsz("blk_pel") % t_tile_sz) == 0 );
	// we assume the out chans are a single (span of) dims in out. FIXME: check this?. FIXME_WXP: what does this even mean?

	//rcg->line( "stores", "  int32_t xpbuf[%(work_out_chan_dim)];\n";
	// FIXME: assumes (for GRP_ID_1D_pels_blk*... term) that input and output block have same # of pels ... too strong?
	assert_st( out.dsz("blk_pel") == in.dsz("blk_pel") );
	rcg->line( "stores", "int32_t const out_ix = (%(GRP_ID_1D_out_chan_blk)*%(work_out_chan_tile_dim)*%(work_out_chan_dim))*%(out_blk_iter_chan_sz) + %(GRP_ID_1D_pels_blk)*%(out_blk_sz);" ); 
	rcg->line( "stores", "int32_t xpbuf_rd_pel;" );
	rcg->line( "stores", "int32_t xpbuf_rd_chan;" );

	for( uint32_t tx = 0; tx != work.dsz("out_chan"); ++tx ) {
	  // transpose each thread's tx'th out_chan (= work_out_chan_dim out chans across all threads) into xpbuf (again across all threads)
	  // such that we can do (mostly) sequential writes to global memory for this set of work_out_chan_dim out chans
	  rcg->line( "stores", "  BARRIER_SYNC;" );
	  for( uint32_t ty = 0; ty != work.dsz("pels"); ++ty ) { // out_tile[] (registers) -> all_smem[]
	    rcg->line( "stores", strprintf( "out_smem_off[%%(tpb)*%s] = %s;", str(ty).c_str(), 
					    add_bias_then_maybe_relu(rcg,work,tx,ty).c_str() ) );
	  }
	  rcg->line( "stores", "  BARRIER_SYNC;" );
	  for( uint32_t ty = 0; ty != work.dsz("pels"); ++ty ) { // all_smem[] -> [xpbuf[] (registers)] -> out[] (global)
	    // here, we reshape the threads so that the total threads across iterations (%(tbp)*work.dsz("pels")) covers
	    // the space of the data in out_smem as a (simple) 2D array ordered as chan:pel. thus, for each thread, we
	    // have a single chan and pel index that we must read from smem and write to global memory. this mapping is
	    // such that the writes to global memory are somewhat sequential (with jumps at chan boundaries). however,
	    // for the reads from smem we just calculate the correct index and hope for the best. note that the actual
	    // output chan indexes read/written to here are strided by %(work_out_chan_dim) and offset by tx.
	    string const obe = "(LOC_ID_1D + %(tpb)*"+str(ty)+")";
	    rcg->line( "stores", "  xpbuf_rd_pel = "+obe+" %% %(out_blk_pel_dim) ;" );
	    rcg->line( "stores", "  xpbuf_rd_chan = "+obe+" / %(out_blk_pel_dim) ;" );
	    rcg->line( "stores", strprintf( "out[out_ix + xpbuf_rd_pel + (xpbuf_rd_chan*%%(work_out_chan_dim)+%s)*%%(out_blk_iter_chan_sz)] = "
					      "all_smem[xpbuf_rd_chan+(xpbuf_rd_pel %%%% %%(work_pels_dim))*%%(tpb)"
					      "+ (xpbuf_rd_pel / %%(work_pels_dim))*%%(work_out_chan_tile_dim) ];",
					      str(tx).c_str() ) );
	  }
	  for( uint32_t ty = 0; ty != work.dsz("pels"); ++ty ) { // xpbuf[] registers -> out[] (global)
	    // TODO/UNUSED?
	  }	
	}
      } else {
	rcg->line( "stores", "  int32_t tpix[%(work_pels_dim)];" );
	rcg->line( "stores", "  int32_t tcix[%(work_out_chan_dim)];" );
	for( uint32_t ty = 0; ty != work.dsz("pels"); ++ty ) { 
	  insert_nda_ix_exprs( rcg->str_vals, "out_pel_" + str(ty), must_find(rcg->all_ix_dims,"out_ref_pel"),
			       "( (%(GRP_ID_1D_pels_blk)*%(work_pels_tile_dim) + %(LOC_ID_1D_pels_tile))*%(work_pels_dim) + "+str(ty)+" )" );
	  rcg->line( "stores", strprintf( "  tpix[%s] = %%(out_pel_%s_img)*%%(out_img_sz) + "
					    " %%(out_pel_%s_x)*%%(out_x_sz) + %%(out_pel_%s_y)*%%(out_y_sz) " // FIXME_WXP:restore: y:x adj-dim opt?
					    "  ; // cache out pel ixs",
					    str(ty).c_str(), str(ty).c_str(), str(ty).c_str(), str(ty).c_str() ) );
	}
	for( uint32_t ty = 0; ty != work.dsz("out_chan"); ++ty ) { 
	  rcg->line( "stores", strprintf( "  tcix[%s] = (%%(out_chan_ix)+%s)*%%(out_chan_sz); // cache out chan ixs",
					    str(ty).c_str(), str(ty).c_str() ) );
	}
	for( uint32_t ty = 0; ty != work.dsz("pels"); ++ty ) {
	  rcg->line( "stores", "  if( %(out_pel_"+str(ty)+"_img) >= %(out_img_dim) ) { return; } "
		       "// this pel and the following are off-the-end pels, so don't store them." );
	  for( uint32_t tx = 0; tx != work.dsz("out_chan"); ++tx ) {
	    rcg->line( "stores", strprintf( "if( tcix[%s] < (%%(out_chan_dim)*%%(out_chan_sz)) ) { out[ tpix[%s] + tcix[%s] ] = %s; }",
					      str(tx).c_str(), str(ty).c_str(), str(tx).c_str(), 
					    add_bias_then_maybe_relu(rcg,work,tx,ty).c_str() ) );
	  }
	}
      }
      for( uint32_t ty = 0; ty != work.dsz("pels"); ++ty ) {
	for( uint32_t tx = 0; tx != work.dsz("out_chan"); ++tx ) {
	  rcg->line( "dummy_stores", strprintf( "out_off[%s] = %s;", 
						  str((ty*work.dsz("out_chan")+tx)*rcg->tpb).c_str(), 
						add_bias_then_maybe_relu(rcg,work,tx,ty).c_str() ) );
	}
      }
      for( uint32_t tx = 0; tx != work.dsz("out_chan"); ++tx ) {
	rcg->line( "bias_loads", strprintf( "filts_strip[%s] = filts_smem_off[%s*%%(work_out_chan_tile_dim)];", 
					      str(tx).c_str(), str(tx).c_str() ) );
      }
      assert_st( in.dsz("blk_pel") == work.dsz("pels_tile")*work.dsz("pels") ); // by input xform design
      for( uint32_t ict = 0; ict != in.dsz("blk_iter_chan"); ++ict ) {
	for( uint32_t tx = 0; tx != work.dsz("out_chan"); ++tx ) {
	  rcg->line( "inner_loop_body", strprintf( "filts_strip[%s] = filts_smem_off[(%s*%%(filts_in_chan_sz))+%s*%%(work_out_chan_tile_dim)];", 
						     str(tx).c_str(), str(ict).c_str(), str(tx).c_str() ) );
	}
	for( uint32_t ty = 0; ty != work.dsz("pels"); ++ty ) { 
	  rcg->line( "inner_loop_body", strprintf( "in_strip[%s] = in_smem_off[(%s*%%(in_blk_pel_dim)+%s)];",
						     str(ty).c_str(), str(ict).c_str(), str(ty).c_str() ) );
	}
	for( uint32_t ty = 0; ty != work.dsz("pels"); ++ty ) {
	  for( uint32_t tx = 0; tx != work.dsz("out_chan"); ++tx ) {
	    rcg->line( "inner_loop_body", strprintf( "out_tile[%s] += filts_strip[%s]*in_strip[%s];", 
						       str((ty*work.dsz("out_chan")+tx)).c_str(), str(tx).c_str(), str(ty).c_str() ) );
	  }
	}
      }
      rcg->has_final_flags_arg = 1;
    }

    void gen_op_tconv( rtc_call_gen_t * rcg ) {
      dims_t const & stride = rcg->get_arg_dims_by_name( "stride" );
      dims_t const & work = rcg->get_arg_dims_by_name( "work" );
      dims_t const & filts = rcg->get_arg_dims_by_name( "filts" );
      dims_t const & in = rcg->get_arg_dims_by_name( "in" );
      uint32_t const filts_smem_sz = filts.dstride("y");
      rcg->set( "filts_smem_sz", str(filts_smem_sz) );
      gen_filts_smem_loads( rcg, filts_smem_sz );
      rcg->line( "filts_smem_loads", "filts_off += %(filts_smem_sz);" );
      uint32_t const in_smem_load_iter = u32_ceil_div( in.dstride("blk_in_chan"), rcg->tpb );  // in smem loads
      for( uint32_t i = 0; i != in_smem_load_iter; ++i ) {
	string const ixe = "(LOC_ID_1D + %(tpb) * "+str(i)+")";
	string eif;
	if( (i+1)*rcg->tpb > in.dstride("blk_in_chan") ) { rcg->line( "in_smem_loads", "if( "+ixe+" < %(in_blk_in_chan_sz)) { " );eif = "}";}
	rcg->line( "in_smem_loads", strprintf("in_smem[%s] = in[ blk_in_ix_base + (%%(tpb)*%s) ];%s",	ixe.c_str(),str(i).c_str(),eif.c_str()));
      }
      rcg->line( "in_smem_loads", "blk_in_ix_base += %(in_blk_in_chan_sz);" );

      for( uint32_t i = 0; i != in.dsz("blk_x"); ++i ) {
	rcg->line( "inner_loop_body", strprintf( "in_strip[%s] = in_smem_off[%s];", str(i).c_str(), str(i).c_str() ) );
      }
      assert_st( work.dsz("out_chan_tile") == filts.dsz("out_chan_tile") ); // also == %(filts_out_chan_reg_sz)
      for( uint32_t kx = 0; kx != filts.dsz("x"); ++kx ) {
	for( uint32_t tx = 0; tx != work.dsz("out_chan"); ++tx ) {
	  rcg->line( "inner_loop_body", strprintf( "filts_strip[%s] = filts_smem_off[%s*%%(filts_x_sz)+%s*%%(filts_out_chan_reg_sz)];", 
						     str(tx).c_str(), str(kx).c_str(), str(tx).c_str() ) );
	}
	for( uint32_t ty = 0; ty != work.dsz("pels"); ++ty ) {
	  for( uint32_t tx = 0; tx != work.dsz("out_chan"); ++tx ) {
	    rcg->line( "inner_loop_body", strprintf( "out_tile[%s] += filts_strip[%s]*in_strip[%s];",
						     str((ty*work.dsz("out_chan")+tx)).c_str(), 
						     str(tx).c_str(), str(ty*stride.dsz("x")+kx).c_str()));
	  }
	}
      }
      for( uint32_t tx = 0; tx != work.dsz("out_chan"); ++tx ) {
	rcg->line( "bias_loads", strprintf( "filts_strip[%s] = filts_smem_off[%s*%%(filts_out_chan_reg_sz)];", str(tx).c_str(), str(tx).c_str() ) );
      }
      //rcg->line( "stores", "  if( %(out_line_y) >= %(out_ix_y_sz) ) { return; }" ); // not possible
      rcg->line( "stores", "if( %(out_line_img) >= %(out_img_dim) ) { return; }" );
      rcg->line( "stores", "int32_t out_x = %(GRP_ID_1D_blk_bx)*%(work_pels_dim);" );
      rcg->line( "stores", "int32_t out_chan = (%(GRP_ID_1D_out_chan_blk)*%(work_out_chan_tile_dim) + %(LOC_ID_1D_out_chan_tile))*%(work_out_chan_dim);" );
      rcg->line( "stores", "GASQ float * out_off = out + %(out_line_img)*%(out_img_sz) + out_chan*%(out_chan_sz) + "
		   "%(out_line_y)*%(out_y_sz) + out_x*%(out_x_sz) ;" );

      for( uint32_t ty = 0; ty != work.dsz("pels"); ++ty ) {
	rcg->line( "stores", "if( (out_x + "+str(ty)+") >= %(out_x_dim) ) { return; } "
		     "// this x value and the following are off-the-end pels, so don't store them." );
	for( uint32_t tx = 0; tx != work.dsz("out_chan"); ++tx ) {
#if 1
	  string const ve = add_bias_then_maybe_relu(rcg,work,tx,ty);

#else
	  string const ve = strprintf( "(filts_strip[%s])", str(tx).c_str() );
#endif
	  rcg->line( "stores", strprintf( "if( (out_chan + %s) < %%(out_chan_dim) ) { "
						   "out_off[ %s*%%(out_chan_sz) + %s*%%(out_x_sz) ] = %s; }",
						   str(tx).c_str(), str(tx).c_str(), str(ty).c_str(), ve.c_str() ) );
	}
      }
      rcg->has_final_flags_arg = 1;
    }
  };

  p_custom_codegen_t make_cnn_custom_codegen_t( void ) { return p_custom_codegen_t( new cnn_custom_codegen_t ); }

}
