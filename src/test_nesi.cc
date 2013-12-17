#include"boda_tu_base.H"
#include"has_main.H"
#include"pyif.H"
#include"str_util.H"
#include"xml_util.H"
#include"lexp.H"
#include"nesi.H"
#include"timers.H"
#include<boost/regex.hpp>
#include<boost/filesystem.hpp>
#include<boost/iostreams/device/mapped_file.hpp>
#include"dtl/dtl.hpp"
#include"img_io.H"
#include"test_base.H"

namespace boda 
{
  using pugi::xml_node;
  using pugi::xml_document;

  using boost::regex;
  using boost::regex_search;

  using boost::filesystem::path;
  using boost::filesystem::exists;
  using boost::filesystem::is_directory;
  using boost::filesystem::is_regular_file;
  using boost::filesystem::filesystem_error;
  using boost::filesystem::recursive_directory_iterator;

  struct various_stuff_t;
  typedef shared_ptr< various_stuff_t > p_various_stuff_t;
  typedef vector< p_various_stuff_t > vect_p_various_stuff_t;
  typedef vector< double > vect_double;
  typedef vector< uint64_t > vect_uint64_t;
  typedef shared_ptr< double > p_double;
  typedef shared_ptr< string > p_string;
  struct one_p_string_t : public virtual nesi // NESI(help="struct with one p_string")
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support
    p_string s; // NESI(help="foo")
  };
  typedef vector< one_p_string_t > vect_one_p_string_t;
  struct various_stuff_t : public virtual nesi, public has_main_t // NESI(help="test of various base types in nesi", bases=["has_main_t"], type_id="vst", hide=1 )
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support
    uint64_t u64; //NESI(help="a u64 with a default",default="345")
    double dpf; //NESI(req=1)
    double dpf_nr; //NESI(default="233.5")
    vect_double vdpf; //NESI()
    p_double pdpf; //NESI()
    vect_uint64_t vu64; //NESI()
    vect_one_p_string_t vops; //NESI()
    one_p_string_t ops; //NESI()
    vect_string vstr; //NESI()
    filename_t fn; //NESI(default="yo.mom")
    virtual void main( nesi_init_arg_t * nia ) {
      //printf("vst::main()\n");
    }
  };

  struct sub_vst_t : public virtual nesi, public various_stuff_t // NESI(help="sub type of vst", bases=["various_stuff_t"], type_id="sub_vst")
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support

  };

  struct sub_vst_2_t : public virtual nesi, public various_stuff_t // NESI(help="sub type of vst", bases=["various_stuff_t"], type_id="sub_vst_2",tid_vn="sub_mode")
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support
    string sub_mode; //NESI(help="name of sub_mode to run",req=1)
  };

  struct sub_sub_vst_2_t : public virtual nesi, public various_stuff_t // NESI(help="sub type of vst", bases=["sub_vst_2_t"], type_id="sub_sub_vst_2")
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support
  };


  struct nesi_test_t
  {
    string name;
    string desc;
    string in;
    char const * err_fmt;
  };

  string const ntb( "(mode=vst,boda_output_dir=." ); // note unmatched open paren ...
  string const ntb_fn = ntb + ",dpf=3.4,fn=";
  nesi_test_t nesi_tests[] = {
    { "bad_mode", "", "(mode=foozledefoo)", "error: type id str of 'foozledefoo' did not match any derived class of has_main_t\n"},
    { "vect_init_t1", "", ntb+",dpf=3.4,vdpf=(li_0=23.4))", 0},
    { "vect_init_t2", "", ntb+",dpf=3.4,vdpf=23.4)", "var 'vdpf': error: invalid attempt to use string as name/value list for vector init. string was:23.4"},
    { "no_req_val_t3", "", ntb+")", "error: missing required value for var 'dpf'"},
    { "bad_list_as_val_t1", "", ntb+",dpf=(li_0=3.4,li_1=34.0))", "var 'dpf': error: invalid attempt to use name/value list as double (double precision floating point number) value. list was:(li_0=3.4,li_1=34.0)"},
    { "bad_val_t1", "", ntb+",dpf=2jj2)", "var 'dpf': error: can't convert '2jj2' to double (double precision floating point number)."},
    { "bad_val_t2", "", ntb+",dpf=23.1,vstr=(li_0=sdf,li_1=(li_0=biz)))", "var 'vstr': list elem 1: error: invalid attempt to use name/value list as string value. list was:(li_0=biz)"},
    { "fn_t1", "", ntb_fn+"foo.txt)", 0},
    { "fn_t2", "", ntb_fn+"%(boda_output_dir)/foo.txt)", 0},
    { "fn_t3", "", ntb_fn+"%(boda_test_dir)/foo.txt)", "var 'fn': error: unable to expand ref 'boda_test_dir' in filename, ref not found"}, // note: boda_test_dir is generally valid, but should indeed be unavailable when run in this context
    { "fn_t4", "ref_not_found", ntb_fn+"%(higgy_ma_jiggy)/foo.txt)", "var 'fn': error: unable to expand ref 'higgy_ma_jiggy' in filename, ref not found"},
    { "fn_t5", "escaped_percent", ntb_fn+"20%%_cooler.txt)", 0},
    { "fn_t6", "bad_percent", ntb_fn+"20%_cooler.txt)", "var 'fn': error: '_' after '%' in filename, expected '(' or '%'."},
    { "fn_t7", "percent_at_end", ntb_fn+"20%)", "var 'fn': error: end of string after '%' in filename, expected '(' or '%'."},
    { "fn_t8", "percent_op_at_end", ntb_fn+"20%\\()", "var 'fn': error: end of string after '%(' in filename, expected ')' to terminate ref"}, // yeah, good luck getting this error in the wild.
    { "fn_t9", "", ntb_fn+"%(av)/foo.txt,av=(li_0=foo))", "var 'fn': error: invalid attempt to use name/value list as filename ref 'av' value. list was:(li_0=foo)" },
    
  };

  extern tinfo_t tinfo_p_has_main_t;

  struct nesi_test_run_t : public test_run_t, public virtual nesi, public has_main_t // NESI(help="NESI initialization tests (error handling and correct usages).", bases=["has_main_t"], type_id="test_nesi", hide=1 )
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support

    void test_print( void ) {
      printf( "tix=%s nesi_tests[tix].desc=%s\n", str(tix).c_str(), str(nesi_tests[tix].desc).c_str() );
    }

    void nesi_test_run( void )
    {
      // note: global parse_lexp() function is inlined here for customization / error generation
      nesi_test_t const & lt = nesi_tests[tix];
      p_lexp_t lexp = parse_lexp( lt.in ); // nesi tests should be valid lexps (otherwise they should be a lexp test)
      bool no_error = 0;
      p_has_main_t has_main;
      try {
	nesi_init_and_check_unused_from_lexp( 0, &tinfo_p_has_main_t, &has_main, lexp ); 
	no_error = 1;
      } catch( rt_exception const & rte ) {
	assert_st( !no_error );
	if( !lt.err_fmt ) { // expected no error, but got one
	  test_fail_err( rte.err_msg ); 
	  // (*ofs_open("/tmp/eib.txt")) << rte.err_msg; // poor-man's auto-update
	} 
	else { 	// check if error is correct one
	  string const exp_err_msg = string(lt.err_fmt); 
	  if( rte.err_msg != exp_err_msg ) { 
	    test_fail_wrong_err( strprintf( "  %s\nexpected:\n  %s\n", 
					    str(rte.err_msg).c_str(), str(exp_err_msg).c_str() ) );
	    // (*ofs_open("/tmp/eib.txt")) << rte.err_msg; // poor-man's auto-update
	  }
	}
      }
      // (insert-file "/tmp/eib.txt")
      if( no_error ) {
	assert_st( has_main );
	if( lt.err_fmt ) { test_fail_no_err( string(lt.err_fmt) ); }
	else { 
	  // no error expected, no error occured. check that the string 
	  string const nesi_to_str( str( *has_main ) );
	  if( nesi_to_str != lt.in ) { test_fail_wrong_res( strprintf( "nesi_to_str=%s != lt.in=%s\n", 
								       str(nesi_to_str).c_str(), str(lt.in).c_str() ) );
	  }	
	}
      }
    }
    void main( nesi_init_arg_t * nia ) {
      num_fail = 0;
      for( tix = 0; tix < ( sizeof( nesi_tests ) / sizeof( nesi_test_t ) ); ++tix ) { nesi_test_run(); }
      if( num_fail ) { printf( "nesi_test num_fail=%s\n", str(num_fail).c_str() ); }
    }
  };


  struct cmd_test_t : public virtual nesi // NESI(help="nesi test case")
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support
    string test_name; //NESI(help="name of test",req=1)
    p_string err; //NESI(help="expected error (if any)")
    p_has_main_t command; //NESI(help="input",req=1)
  };
  typedef vector< cmd_test_t > vect_cmd_test_t; 
  typedef shared_ptr< cmd_test_t > p_cmd_test_t; 
  typedef vector< p_cmd_test_t > vect_p_cmd_test_t;

#include"nesi_decls.H"

  void run_system_cmd( string const &cmd, bool const verbose ) {
    if( verbose ) { printstr( cmd + "\n" ); }
    int const sys_ret = system( cmd.c_str() );
    assert_st( sys_ret == 0 );
  }

  // get size() of a path by iterating
  uint32_t num_elems( path const & p ) {
    uint32_t p_sz = 0; for( path::iterator pi = p.begin(); pi != p.end(); ++pi ) { ++p_sz; }
    return p_sz;
  }

  // return a new path that is p with the p_sz first elements removed. p must have at least p_sz elements.
  path strip_prefix( uint32_t const p_sz, path const & p ) {
    path rp; 
    path::iterator pi = p.begin();
    for( uint32_t pix = 0; pix != p_sz; ++pix ) { assert_st( pi != p.end() ); ++pi; } 
    for( ; pi != p.end(); ++pi ) { rp /= *pi; }
    return rp;
  }    

  void tag_dir_files( map_str_ziu32_t & tags, path const & p, uint32_t tag ) {
    assert_st( tag < 32 ); // 32 tags supported
    uint32_t const p_sz = num_elems(p);
    for( recursive_directory_iterator i(p); i != recursive_directory_iterator(); ++i ) {
      tags[strip_prefix( p_sz, i->path() ).string()].v |= (1<<tag);
    }
  }


  // this has gotta be in the c++ std lib or boost somewhere ...
  template< typename T >
  struct range {
    typedef T * iterator;
    typedef T const * const_iterator;
    T *b;
    T *e;
    T *begin( void ) { return b; }
    T *end( void ) { return e; }
    uint32_t size( void ) const { return e-b; }
    T &operator[]( size_t const & i ) { return b[i]; }
    range( T * const & b_, T * const & e_ ) : b(b_), e(e_) { }
    bool operator == ( range const & o ) const { if( size()!=o.size() ) { return 0; } return std::equal( b, e, o.b ); }
  };
  typedef range< uint8_t > range_uint8_t;
  typedef range< char const > range_char;
  typedef vector< range_char > vect_range_char;
  std::ostream & operator<<(std::ostream & os, range_char const & v) { os.write( v.b, v.size()); os.flush(); return os; }
  

  // split s at each newline. output will have (# newlines in s) + 1 elements. removes newlines.
  void getlines( vect_range_char & lines, range_char & s ) {
    char const * cur_b = s.begin();
    for( char const * c = s.begin(); c != s.end(); ++c ) {
      if( *c == '\n' ) { lines.push_back( range_char( cur_b, c ) ); cur_b = c+1; } // omit newline
    }
    lines.push_back( range_char( cur_b, s.end() ) ); // note: final elem may be empty and never has a newline
  }


  // returns 1 if files differ
  bool diff_file( path const & good, path const & test, string const & fn ) {
    string const good_fn = (good / fn).string();
    string const test_fn = (test / fn).string();
    assert_st( exists( good_fn ) && exists( test_fn ) );
    if( is_directory( good_fn ) != is_directory( test_fn ) ) {
      printf( "DIFF: directory / non-directory mismatch for file '%s'.", fn.c_str() );
      return 1;
    }
    if( is_directory( good_fn ) ) { return 0; } // both are directories, so that's all fine and well.
    // we can only handle regular files and directories, so check for that:
    assert_st( is_regular_file( good_fn ) && is_regular_file( test_fn ) ); 

    p_mapped_file_source good_map = map_file_ro( good_fn );
    p_mapped_file_source test_map = map_file_ro( test_fn );
    range_char good_range( good_map->data(), good_map->data() + good_map->size() );
    range_char test_range( test_map->data(), test_map->data() + test_map->size() );
    if( endswith(fn, ".txt" ) ) { // do line-by-line diff
      vect_range_char good_lines; getlines( good_lines, good_range );
      vect_range_char test_lines; getlines( test_lines, test_range );
      dtl::Diff< range_char, vect_range_char > d( good_lines, test_lines );
      d.compose();
      if( d.getEditDistance() ) {
	printf( "DIFF: text file '%s' edit distance:%s\n", fn.c_str(), str(d.getEditDistance()).c_str() );
	//d.printSES();
	d.composeUnifiedHunks(); 
	d.printUnifiedFormat();
	return 1;
      }
      else { return 0; }
    } else if( endswith( fn, ".png" ) || endswith( fn, ".jpg" ) ) { // image diff
      if( !( good_range == test_range ) ) { // if not binary identical
	img_t good_img;
	img_t test_img;
	good_img.load_fn( good_fn );
	test_img.load_fn( test_fn );

	dtl::Diff< uint8_t, range_uint8_t > d( 
	  range_uint8_t( good_img.pels.get(), good_img.pels.get()+good_img.sz_raw_bytes() ),
	  range_uint8_t( test_img.pels.get(), test_img.pels.get()+test_img.sz_raw_bytes() ) );
	d.compose();
	printf( "DIFF: image file '%s' edit distance (padded raw color bytes, inexact):%s\n", 
		fn.c_str(), str(d.getEditDistance()).c_str() );
	return 1;
      }
      else { return 0; }
    } else { // bytewise binary diff
      dtl::Diff< char, range_char > d( good_range, test_range );
      d.compose();
      if( d.getEditDistance() ) {
	printf( "DIFF: binary file '%s' edit distance:%s\n", fn.c_str(), str(d.getEditDistance()).c_str() );
	return 1;
      }
      else { return 0; }
    }
    
  }

  void maybe_remove_dir( path const & dir ) {
    if( exists( dir ) ) {
      assert_st( is_directory( dir ) );
      uint32_t const num_rem = remove_all( dir );
      assert( num_rem );
    }
  }

  extern tinfo_t tinfo_vect_p_cmd_test_t;
  struct test_modes_t : public test_run_t, public virtual nesi, public has_main_t // NESI(help="test of modes in various configurations", bases=["has_main_t"], type_id="test_modes" )
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support
    filename_t xml_fn; //NESI(default="%(boda_test_dir)/test_modes.xml",help="xml file containing list of tests.")
    vect_p_cmd_test_t tests; //NESI(help="populated via xml_fn")
    string filt; //NESI(default=".*",help="regexp over test name of what tests to run (default runs all tests)")
    uint32_t verbose; //NESI(default=0,help="if true, print each test lexp before running it")
    uint32_t update_failing; //NESI(default=0,help="if true, update archives for all run tests that fail.")
    
    string boda_test_dir; //NESI(help="boda base test dir (generally set via boda_cfg.xml)",req=1)

    void test_print( void ) {
      printf( "tix=%s %s\n", str(tix).c_str(), str(*tests[tix]).c_str() );
    }

    // returns 1 if command ran with no error and no error was expected (and thus diff should be run)
    bool run_command( p_cmd_test_t cmd, nesi_init_arg_t * nia ) {
      bool do_diff = 0;
      bool write_err = 1;
      bool no_error = 0;
      try {
	cmd->command->base_setup(); // note: sets command->boda_output_dir *and* creates it
	cmd->command->main( nia );	
	no_error = 1;
      } catch( rt_exception const & rte ) {
	assert_st( !no_error );
	if( !cmd->err ) { // expected no error, but got one
	  test_fail_err( rte.err_msg ); 
	  if( write_err ) { (*ofs_open("/tmp/eib.txt")) << rte.err_msg; } // poor-man's auto-update 
	} 
	else { 	// check if error is correct one
	  string const exp_err_msg = *cmd->err;
	  if( rte.err_msg != exp_err_msg ) { 
	    test_fail_wrong_err( strprintf( "  %s\nexpected:\n  %s\n", 
					    str(rte.err_msg).c_str(), str(exp_err_msg).c_str() ) );
	    if( write_err ) { (*ofs_open("/tmp/eib.txt")) << rte.err_msg; } // poor-man's auto-update
	  }
	}
      }
      // (insert-file "/tmp/eib.txt")
      if( no_error ) {
	if( cmd->err ) { test_fail_no_err( *cmd->err ); }
	else { do_diff = 1; }
      }
      return do_diff;
    }

    void diff_command( p_cmd_test_t cmd, path const & gen_test_out_dir ) {
      path good_dir = path(boda_output_dir.exp) / "good";
      ensure_is_dir( good_dir.string(), 1 );
      // note: test_out_dir should equivalent to gen_test_out_dir (but not ==). we check that:
      path const test_out_dir( cmd->command->boda_output_dir.exp );
      assert_st( equivalent( test_out_dir, gen_test_out_dir ) );
      if( !exists( test_out_dir ) ) { // test must create its output dir
	rt_err( strprintf( "test '%s' did not create its expected output directory '%s'.", 
			   cmd->test_name.c_str(), test_out_dir.c_str() ) );
      }

      // note: test_good_dir will be relative to the *test_modes* output_dir, which is usually '.'
      path const test_good_dir = good_dir / cmd->test_name; 
      path const test_good_arc = path(boda_test_dir) / "mt_good" / ( cmd->test_name + ".tbz2");
      bool update_archive = 0;
      if( !exists( test_good_arc ) ) {
	printf("NEW_TEST: no existing good results archive for test %s, will generate\n",cmd->test_name.c_str());
	update_archive = 1;
      } else { // achive exists, unpack it
	assert_st( is_regular_file( test_good_arc ) );
	// first, remove test_good_dir if it exists.
	if( exists( test_good_dir ) ) {
	  assert_st( is_directory( test_good_dir ) );
	  uint32_t const num_rem = remove_all( test_good_dir );
	  assert_st( num_rem );
	}
	bool const did_create = ensure_is_dir( test_good_dir.string(), 1 ); // create good dir, must not exists
	assert_st( did_create );
	run_system_cmd( strprintf("tar -C %s -xjf %s",
				  test_good_dir.string().c_str(),test_good_arc.c_str()), 0 );
	// compare good and test directories
	bool output_good = 1;
	map_str_ziu32_t tags;
	tag_dir_files( tags, test_good_dir, 0 );
	tag_dir_files( tags, test_out_dir, 1 );
	for( map_str_ziu32_t::const_iterator i = tags.begin(); i != tags.end(); ++i ) {
	  uint32_t const & tv = i->second.v;
	  if( tv == 1 ) { printf( "DIFF: file '%s' only in known-good output dir.\n", str(i->first).c_str()); 
	    output_good = 0; continue; 
	  }
	  if( tv == 2 ) { printf( "DIFF: file '%s' only in under-test output dir.\n", str(i->first).c_str()); 
	    output_good = 0; continue; 
	  }
	  assert_st( tv == 3 ); // file in both known-good an under-test output dirs
	  if( diff_file( test_good_dir, test_out_dir, i->first ) ) { 
	    printf( "DIFF: file '%s' differs between known-good(-) and under-test(+):\n", str(i->first).c_str()); 
	    output_good = 0; continue; 
	  }
	}
	if( !output_good ) {
	  if( update_failing ) { 
	    printf("AUTOUPDATE: test %s failed, will update.\n",cmd->test_name.c_str());
	    update_archive = 1; 
	  } else {
	    printf("FAIL: test %s failed.\n",cmd->test_name.c_str());
	  }
	}
      }	  
      if( update_archive ) {
	printf("UPDATING good results archive for test %s.\n",cmd->test_name.c_str());
	run_system_cmd( strprintf("tar -C %s -cjf %s .",
				  test_out_dir.string().c_str(),test_good_arc.c_str()), 0 );
      }
    }

    virtual void main( nesi_init_arg_t * nia ) {
      printf( "test modes: verbose=%s\n", str(verbose).c_str() );
      set_string seen_test_names;
      seen_test_names.insert( "good" ); // reserved sub-dir to hold known good results

      regex filt_regex( filt );
      lexp_name_val_map_t nvm;
      nvm.parent = nia;
      nvm.insert_leaf( "boda_output_dir", "%(test_name)" );
      //p_lexp_t boda_test_cfg = parse_lexp_xml_file( filename_t( "%(boda_test_dir)/boda_test_cfg.xml" ) ); // unneeded complexity? NOTE: broken. make path to xml file into NESI var if needed, as with xml_fn?
      //nvm.populate_from_lexp( boda_test_cfg.get() );

      // note: cmd tests should not fail nesi init (otherwise they should be nesi init tests).
      nesi_init_and_check_unused_from_xml_fn( &nvm, &tinfo_vect_p_cmd_test_t, &tests, xml_fn.exp );
      for (vect_p_cmd_test_t::iterator i = tests.begin(); i != tests.end(); ++i) {
	tix = i-tests.begin();
	bool const seen_test_name = !seen_test_names.insert( (*i)->test_name ).second;
	if( seen_test_name ) { rt_err( "duplicate or reserved (e.g. 'good') test name:" + (*i)->test_name ); }
	if( regex_search( (*i)->test_name, filt_regex ) ) {
	  if( verbose ) { std::cout << (**i) << std::endl; }
	  timer_t t("mode_test");
	  // note: no test may be named 'good'
	  path gen_test_out_dir = path(boda_output_dir.exp) / (*i)->test_name;
	  maybe_remove_dir( gen_test_out_dir );
	  bool const do_diff = run_command( *i, &nvm );
	  if( do_diff ) { diff_command( *i, gen_test_out_dir ); }
	}
      }
    }
  };

#include"gen/test_nesi.cc.nesi_gen.cc"
}

