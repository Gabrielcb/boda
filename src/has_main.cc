#include"boda_tu_base.H"
#include"has_main.H"
#include"lexp.H"
#include"nesi.H"
#include"pyif.H"
#include"str_util.H"
#include<boost/filesystem.hpp>

namespace boda
{
  extern tinfo_t tinfo_p_has_main_t;

  using boost::filesystem::path;
  using boost::filesystem::canonical;

  void has_main_t::base_setup( void ) {
    ensure_is_dir( boda_output_dir.exp, 1 );
  }

  void create_and_run_has_main_t( p_lexp_t lexp ) {
    // add top-level extra fields
    p_lexp_t boda_cfg = parse_lexp_xml_file( (path(py_boda_dir()) / "lib" / "boda_cfg.xml").string() );
    lexp_name_val_map_t nvm;
    nvm.populate_from_lexp( boda_cfg.get() );
    // these won't insert if the field exists
    nvm.insert_leaf( "boda_dir", py_boda_dir().c_str() ); 
    nvm.insert_leaf( "boda_test_dir", py_boda_test_dir().c_str() );
    // create and run mode. note: the unused fields check doesn't apply to 'parent' init data (i.e. nvm), only to lexp
    p_has_main_t has_main;
    nesi_init_and_check_unused_from_lexp( &nvm, &tinfo_p_has_main_t, &has_main, lexp ); 
    has_main->base_setup(); // prior to running main, run shared has_main_t setup.
    has_main->main( &nvm );
  }
#include"gen/has_main.H.nesi_gen.cc"
}
