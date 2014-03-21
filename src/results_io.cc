// Copyright (c) 2013-2014, Matthew W. Moskewicz <moskewcz@alumni.princeton.edu>; part of Boda framework; see LICENSE
#include"boda_tu_base.H"
#include"results_io.H"
#include"str_util.H"
#include"geom_prim.H"
#include<cassert>
#include<string>
#include<map>
#include<vector>
#include<boost/algorithm/string.hpp>
#include<boost/filesystem.hpp>
#include"xml_util.H"
#include"pyif.H"
#include"img_io.H"
#include"octif.H"
#include"timers.H"

namespace boda 
{
  using namespace std;
  using namespace boost;
  using filesystem::path;
  using filesystem::filesystem_error;
  using namespace pugi;

  typedef vector< string > vect_string;
  typedef map< string, uint32_t > str_uint32_t_map_t;
  string id_from_image_fn( string const & image )
  {
    size_t const last_dot = image.find_last_of('.');	
    string id = image.substr(0, last_dot);
    size_t const last_slash = id.find_last_of("/\\");
    if (last_slash != string::npos) {
      id = id.substr(last_slash + 1);
    }
    return id;
  }

  std::ostream & operator <<(std::ostream & os, const scored_det_t & v) {
    return os << ((u32_box_t &)v) << "@" << v.img_ix << "=" << v.score; }

  struct scored_det_t_comp_by_inv_score_t { 
    bool operator () ( scored_det_t const & a, scored_det_t const & b ) const { return a.score > b.score; } };
  
  struct gt_det_t : public u32_box_t
  {
    uint32_t truncated;
    uint32_t difficult;    
    gt_det_t( void ) : truncated(0), difficult(0) { }
  };

  std::ostream & operator <<(std::ostream & os, const gt_det_t & v) {
    return os << ((u32_box_t &)v) << ":T" << v.truncated << "D" << v.difficult; }
  struct vect_gt_det_t : public vector< gt_det_t >
  {
    zi_uint32_t num_non_difficult;
  };
  typedef map< string, vect_gt_det_t > name_vect_gt_det_map_t;

  struct img_info_t
  {
    string id;
    string full_fn;
    uint32_t ix;
    u32_pt_t size;
    uint32_t depth;
    name_vect_gt_det_map_t gt_dets;

    p_img_t img;
    img_info_t( string const & id_ ) : id(id_), ix( uint32_t_const_max ), depth(0) { }
  };
  typedef shared_ptr< img_info_t > p_img_info_t;
  typedef map< string, p_img_info_t > id_to_img_info_map_t;
  typedef vector< p_img_info_t > vect_p_img_info_t;

  void read_pascal_image_for_id( p_img_info_t img_info, path const & img_dir )
  {
    ensure_is_dir( img_dir.string() );
    img_info->full_fn = string( (img_dir / (img_info->id + ".jpg")).c_str() );
    img_info->img.reset( new img_t );
    img_info->img->load_fn( img_info->full_fn.c_str() );
  }

  void read_pascal_annotations_for_id( p_img_info_t img_info, path const & ann_dir, string const & id )
  {
    ensure_is_dir( ann_dir.string() );
    path const ann_path = ann_dir / (id + ".xml");
    ensure_is_regular_file( ann_path.string() );
    char const * const ann_fn = ann_path.c_str();
    xml_document doc;
    xml_node ann = xml_file_get_root( doc, ann_fn );
    // assert_st( ann.name() == string("annotation") ); // true, but we don't really care?
    xml_node ann_size = xml_must_decend( ann_fn, ann, "size" );
    img_info->size.d[0] = lc_str_u32( xml_must_decend( ann_fn, ann_size, "width" ).child_value() );
    img_info->size.d[1] = lc_str_u32( xml_must_decend( ann_fn, ann_size, "height" ).child_value() );
    img_info->depth = lc_str_u32( xml_must_decend( ann_fn, ann_size, "depth" ).child_value() );

    for( xml_node ann_obj = ann.child("object"); ann_obj; ann_obj = ann_obj.next_sibling("object") ) {
      gt_det_t gt_det;
      gt_det.truncated = lc_str_u32( xml_must_decend( ann_fn, ann_obj, "truncated" ).child_value() );
      gt_det.difficult = lc_str_u32( xml_must_decend( ann_fn, ann_obj, "difficult" ).child_value() );
      xml_node ann_obj_bb = xml_must_decend( ann_fn, ann_obj, "bndbox" );
      gt_det.p[0].d[0] = lc_str_u32( xml_must_decend( ann_fn, ann_obj_bb, "xmin" ).child_value() );
      gt_det.p[0].d[1] = lc_str_u32( xml_must_decend( ann_fn, ann_obj_bb, "ymin" ).child_value() );
      gt_det.p[1].d[0] = lc_str_u32( xml_must_decend( ann_fn, ann_obj_bb, "xmax" ).child_value() );
      gt_det.p[1].d[1] = lc_str_u32( xml_must_decend( ann_fn, ann_obj_bb, "ymax" ).child_value() );
      gt_det.from_pascal_coord_adjust();
      assert_st( gt_det.is_strictly_normalized() );
      string const ann_obj_name( xml_must_decend( ann_fn, ann_obj, "name" ).child_value() );
      vect_gt_det_t & gt_dets = img_info->gt_dets[ann_obj_name];
      gt_dets.push_back(gt_det);
      if( !gt_det.difficult ) { ++gt_dets.num_non_difficult.v; } 
    }
  }

  typedef map< string, zi_uint32_t > class_infos_t;

  struct match_res_t
  {
    bool is_pos;
    bool is_diff;
    match_res_t( bool const is_pos_, bool const is_diff_ ) : is_pos(is_pos_), is_diff(is_diff_) { }
  };


  struct img_db_t : virtual public nesi // NESI(help="store images and detections")
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support
    filename_t pascal_ann_dir; // NESI(default="%(pascal_data_dir)/Annotations",help="pascal annotations directory")
    filename_t pascal_img_dir; // NESI(default="%(pascal_data_dir)/JPEGImages",help="pascal images directory")
    id_to_img_info_map_t id_to_img_info_map;
    vect_p_img_info_t img_infos;
    class_infos_t class_infos;

    // note: expects that only a single image list will be loaded
    // (with check_ix_only=0), and that any subsequent lists loaded
    // (with check_ix_only=1) will have the same images in the same
    // order. this strictness is primarily for error checking can be
    // relaxed as needed.
    void load_pascal_data_for_id( string const & img_id, bool load_img, uint32_t const in_file_ix, bool check_ix_only )
    {
      p_img_info_t & img_info = id_to_img_info_map[img_id];
      if( check_ix_only ) {
	if( !img_info ) { rt_err( "expected image to already be loaded but it was not" ); }
	if( img_info->ix != in_file_ix ) { rt_err( strprintf( "already-loaded image had ix=%s, but expected %s",
							      str(img_info->ix).c_str(), str(in_file_ix).c_str() ) ); }
	return;
      }
      if( img_info ) { rt_err( "tried to load annotations multiple times for id '"+img_id+"'"); }
      img_info.reset( new img_info_t( img_id ) );
      read_pascal_annotations_for_id( img_info, pascal_ann_dir.exp, img_id ); 
      if( load_img ) { read_pascal_image_for_id( img_info, pascal_img_dir.exp ); }

      img_info->ix = img_infos.size();
      if( img_info->ix != in_file_ix ) { rt_err( strprintf( "newly-loaded image had ix=%s, but expected %s",
							    str(img_info->ix).c_str(), str(in_file_ix).c_str() ) ); }
      img_infos.push_back( img_info );
      for( name_vect_gt_det_map_t::const_iterator i = img_info->gt_dets.begin(); i != img_info->gt_dets.end(); ++i )
      {
	class_infos[i->first].v += i->second.num_non_difficult.v;
      }
    }
    string get_id_for_img_ix( uint32_t const img_ix )
    {
      assert( img_ix < img_infos.size() );
      return img_infos[img_ix]->id;
    }
    uint32_t get_ix_for_img_id( string const & img_id )
    {
      id_to_img_info_map_t::iterator img_info = id_to_img_info_map.find(img_id);
      if( img_info == id_to_img_info_map.end() ) { rt_err("tried to get ix for unloaded img_id '"+img_id+"'"); }
      return img_info->second->ix;
    }
    match_res_t try_match( string const & class_name, p_vect_scored_det_t scored_dets, scored_det_t const & sd );
    void score_results_for_class( p_vect_scored_det_t name_scored_dets, 
				  string const & prc_txt_fn, string const & prc_png_fn );
    void score_results( p_vect_p_vect_scored_det_t name_scored_dets_map, 
			string const & prc_fn, string const & plot_base_fn );

  };
  typedef shared_ptr< img_db_t > p_img_db_t;

  void img_db_show_dets( p_img_db_t img_db, p_vect_scored_det_t scored_dets, uint32_t img_ix )
  {
    assert_st( img_ix < img_db->img_infos.size() );
    p_img_info_t img_info = img_db->img_infos[img_ix];
    if( !img_info->img ) { read_pascal_image_for_id( img_info, img_db->pascal_img_dir.exp ); }
    show_dets( img_info->img, scored_dets );
  }

  p_vect_scored_det_t read_results_file( p_img_db_t img_db, string const & fn, string const & class_name )
  {
    timer_t t("read_results_file");
    p_ifstream in = ifs_open( fn );  
    string line;
    p_vect_scored_det_t scored_dets( new vect_scored_det_t( class_name ) );
    while( !ifs_getline( fn, in, line ) )
    {
      vect_string parts;
      split( parts, line, is_space(), token_compress_on );
      if( (parts.size() == 1) && parts[0].empty() ) { continue; } // skip ws-only lines
      assert( parts.size() == 6 );
      scored_det_t scored_det;
      string const img_id = parts[0];
      scored_det.img_ix = img_db->get_ix_for_img_id( img_id );
      scored_det.score = lc_str_d( parts[1] );
      scored_det.p[0].d[0] = lc_str_u32( parts[2] );
      scored_det.p[0].d[1] = lc_str_u32( parts[3] );
      scored_det.p[1].d[0] = lc_str_u32( parts[4] );
      scored_det.p[1].d[1] = lc_str_u32( parts[5] );
      scored_det.from_pascal_coord_adjust();
      assert_st( scored_det.is_strictly_normalized() );
      scored_dets->push_back( scored_det );
    }
    return scored_dets;
  }

  void write_results_file( p_img_db_t img_db, string const & fn, p_vect_scored_det_t scored_dets )
  {
    p_ofstream out = ofs_open( fn );  
    for (vect_scored_det_t::const_iterator i = scored_dets->begin(); i != scored_dets->end(); ++i)
    {
      string const img_id = img_db->get_id_for_img_ix(i->img_ix);
      (*out) << strprintf( "%s %s %s\n", str(img_id).c_str(), str(i->score).c_str(), i->pascal_str().c_str() );
    }
    out->close();
  }

  struct read_pascal_image_list_file_t : virtual public nesi, public has_main_t // NESI(help="read a pascal-VOC-format image list file",bases=["has_main_t"], type_id="load_pil")
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support
    filename_t pil_fn; //NESI(help="name of pascal-VOC format image list file",req=1)
    vect_filename_t check_pil_fns; //NESI(help="name of pascal-VOC format image list files to check image-set agreement (same images in same order) against the primary one")
    uint32_t load_imgs; //NESI(default=0,help="if true, load images referenced by the file")
    p_img_db_t img_db; //NESI(default="()", help="image database")
    virtual void main( nesi_init_arg_t * nia ) { 
      read_pascal_image_list_file( img_db, pil_fn.exp, false, 0 ); 
      for( vect_filename_t::const_iterator i = check_pil_fns.begin(); i != check_pil_fns.end(); ++i ) {
	read_pascal_image_list_file( img_db, i->exp, false, 1 );
      }
    }
  };

  void read_pascal_image_list_file( p_img_db_t img_db, string const & pil_fn, bool const load_imgs, 
				    bool const check_ix_only )
  {
    timer_t t("read_pascal_image_list_file");
    p_ifstream in = ifs_open( pil_fn );  
    string line;
    uint32_t in_file_ix = 0; // almost (0 based) line number, but only for non-blank lines
    while( !ifs_getline( pil_fn, in, line ) )
    {
      vect_string parts;
      split( parts, line, is_space(), token_compress_on );
      if( (parts.size() == 1) && parts[0].empty() ) { continue; } // skip ws-only lines
      if( parts.size() != 2 ) { 
	rt_err( strprintf( "invalid line in image list file '%s': num of parts != 2 after space "
			   "splitting. line was:\n%s", pil_fn.c_str(), line.c_str() ) ); }
      string const id = parts[0];
      string const pn = parts[1];
      if( (pn != "1") && (pn != "-1") && (pn != "0") ) {
	rt_err( strprintf( "invalid type string in image list file '%s': saw '%s', expected '1', '-1', or '0'.",
			   pil_fn.c_str(), pn.c_str() ) );
      }
      img_db->load_pascal_data_for_id( id, load_imgs, in_file_ix, check_ix_only );
      ++in_file_ix;
    }
  }
  
  struct score_results_file_t : virtual public nesi, public has_main_t // NESI(help="score a pascal-VOC-format results file",bases=["has_main_t"], type_id="score")
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support
    filename_t pil_fn; //NESI(help="input: name of pascal-VOC format image list file",req=1)
    filename_t res_fn; //NESI(help="input: name of pascal-VOC format detection results file to read",req=1)
    filename_t prc_txt_fn; //NESI(default="%(boda_output_dir)/prc_",help="output: text prc curve base filename")
    filename_t prc_png_fn; //NESI(default="%(boda_output_dir)/mAP_",help="output: png prc curve base filename")
    string class_name; //NESI(help="name of object class",req=1)
    p_img_db_t img_db; //NESI(default="()", help="image database")
    virtual void main( nesi_init_arg_t * nia ) {
      read_pascal_image_list_file( img_db, pil_fn.exp, false, 0 );
      p_vect_scored_det_t scored_dets = read_results_file( img_db, res_fn.exp, class_name );
      img_db->score_results_for_class( scored_dets, prc_txt_fn.exp, prc_png_fn.exp );
    }
  };

  struct run_dfc_t : virtual public nesi, public has_main_t // NESI(help="run dpm fast cascade over pascal-VOC-format image file list",bases=["has_main_t"], type_id="run_dfc")
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support

    filename_t pascal_classes_fn; //NESI(default="%(boda_test_dir)/pascal/head_100/pascal_classes.txt",help="file with list of classes to process")
    filename_t pil_fn; //NESI(default="%(boda_test_dir)/pascal/head_100/%%s.txt",help="format for filenames of image list files. %%s will be replaced with the class name")
    p_img_db_t img_db; //NESI(default="()", help="image database")
    filename_t dpm_fast_cascade_dir; //NESI(default="%(dpm_fast_cascade_dir)",help="dpm_fast_cascade base dir")

    filename_t res_fn; //NESI(default="%(boda_output_dir)/%%s.txt",help="output: format for filenames of pascal-VOC format detection results file to write")
    filename_t prc_txt_fn; //NESI(default="%(boda_output_dir)/prc_",help="output: text prc curve base filename")
    filename_t prc_png_fn; //NESI(default="%(boda_output_dir)/mAP_",help="output: png prc curve base filename")


    virtual void main( nesi_init_arg_t * nia ) {
      p_vect_string classes = readlines_fn( pascal_classes_fn.exp );
      p_vect_p_vect_scored_det_t scored_dets( new vect_p_vect_scored_det_t );      
      for( vect_string::const_iterator i = (*classes).begin(); i != (*classes).end(); ++i ) {
	read_pascal_image_list_file( img_db, strprintf( pil_fn.exp.c_str(), (*i).c_str() ), 
				     true, (i != (*classes).begin()) ); 	
      }

      for( vect_string::const_iterator i = (*classes).begin(); i != (*classes).end(); ++i ) {
	scored_dets->push_back( p_vect_scored_det_t( new vect_scored_det_t( *i ) ) );
	for (uint32_t ix = 0; ix < img_db->img_infos.size(); ++ix) {
	  p_img_info_t img_info = img_db->img_infos[ix];
	  oct_dfc( cout, dpm_fast_cascade_dir.exp, scored_dets->back(), img_info->full_fn, img_info->ix );
	}
	string c_res_fn = strprintf( res_fn.exp.c_str(), (*i).c_str() );
	write_results_file( img_db, c_res_fn, scored_dets->back() );
      }
      img_db->score_results( scored_dets, prc_txt_fn.exp, prc_png_fn.exp );

    }
  };

  // returns (matched, match_was_difficult). note that difficult is
  // only seems meaningfull when matched=true, although it is
  // sometimes valid when matched=false (i.e. if the match is false
  // due to being a duplicate, the difficult bit indicates if the
  // higher-scoring-matched gt detection was marked difficult).
  match_res_t img_db_t::try_match( string const & class_name, p_vect_scored_det_t scored_dets, scored_det_t const & sd )
  {
    assert_st( sd.img_ix < img_infos.size() );
    p_img_info_t img_info = img_infos[sd.img_ix];
    vect_gt_det_t & gt_dets = img_info->gt_dets[class_name]; // note: may be created here (i.e. may be empty)
    vect_gt_match_t & gtms = scored_dets->get_gtms( sd.img_ix, gt_dets.size() ); // note: may be created here

    uint64_t const sd_area = sd.get_area();
    u32_pt_t best_score(0,1); // as a fraction [0]/[1] --> num / dem
    gt_match_t * best = 0;
    uint32_t best_difficult = uint32_t_const_max; // cached difficult bit from the gt associated with best
    for( vect_gt_det_t::iterator i = gt_dets.begin(); i != gt_dets.end(); ++i )
    {
      uint64_t const gt_area = i->get_area();
      uint64_t const gt_overlap = i->get_overlap_with( sd );
      uint64_t const gt_sd_union = sd_area + gt_area - gt_overlap;
      // must overlap by 50% in terms of ( intersection area / union area ) (pascal VOC criterion) 
      bool has_min_overlap = ( gt_overlap * 2 ) >= gt_sd_union;
#if 0
      if( img_info->id == "xx001329" )
      {
	printf( "sd=%s gt=%s sd_area=%s gt_area=%s gt_overlap=%s gt_score=%s max_score=%s hmo=%s\n", str(sd).c_str(), str(*i).c_str(),str(sd_area).c_str(), str(gt_area).c_str(), str(gt_overlap).c_str(), str(double(gt_overlap)/gt_sd_union).c_str(), str(double(best_score.d[0])/best_score.d[1]).c_str(), str(has_min_overlap).c_str() );
      }
#endif
      if( !has_min_overlap ) { continue; }

      if( (uint64_t(gt_overlap)*best_score.d[1]) > (uint64_t(best_score.d[0])*gt_sd_union) ) {
	best_score.d[0] = gt_overlap; best_score.d[1] = gt_sd_union; best_difficult = i->difficult;
	best = &gtms.at(i-gt_dets.begin()); }
    }
    if( !best_score.d[0] ) { return match_res_t(0,0); }// no good overlap with any gt_det. note: difficult is unknown here
    if( best->matched ) { assert_st( best->match_score >= sd.score ); return match_res_t(0,best_difficult); }
    best->matched = 1; 
    best->match_score = sd.score;
    return match_res_t(1,best_difficult);
  }
  
  void print_prc_line( p_ostream out, prc_elem_t const & prc_elem, uint32_t const tot_num_class, double const & map )
  {
    (*out) << strprintf( "num_pos=%s num_test=%s score=%.6lf p=%s r=%s map=%s\n", 
			 str(prc_elem.num_pos).c_str(), str(prc_elem.num_test).c_str(), 
			 prc_elem.score,
			 str(prc_elem.get_precision()).c_str(), 
			 str(prc_elem.get_recall(tot_num_class)).c_str(), 
			 str(map).c_str() );
  }


  void img_db_t::score_results_for_class( p_vect_scored_det_t name_scored_dets,
					  string const & prc_txt_fn, string const & prc_png_fn )
  {
    timer_t t("score_results_for_class");
    string const & class_name = name_scored_dets->class_name;
    assert_st( !class_name.empty() );
    p_ostream prc_out = ofs_open( prc_txt_fn + class_name + ".txt" );
    sort( name_scored_dets->begin(), name_scored_dets->end(), scored_det_t_comp_by_inv_score_t() );
    uint32_t tot_num_class = class_infos[class_name].v;
    vect_prc_elem_t prc_elems;
    uint32_t num_pos = 0;
    uint32_t num_test = 0;
    double map = 0;
    uint32_t print_skip = 1 + (tot_num_class / 20); // print about 20 steps in recall
    uint32_t next_print = 1;
    (*prc_out ) << strprintf( "---BEGIN--- class_name=%s tot_num_class=%s name_scored_dets->size()=%s\n", str(class_name).c_str(), str(tot_num_class).c_str(), str(name_scored_dets->size()).c_str() );
    for( vect_scored_det_t::const_iterator i = name_scored_dets->begin(); i != name_scored_dets->end(); )
    {
      uint32_t const orig_num_pos = num_pos;
      double const cur_score = i->score;
      while( 1 ) // handle all dets with equal score as a unit
      {
	match_res_t const match_res = try_match( class_name, name_scored_dets, *i );
	if( match_res.is_pos && !match_res.is_diff ) { 
	  ++num_test; ++num_pos; // positive, recall increased (precision must == 1 or also be increased)
	} else if( !match_res.is_pos ) { ++num_test; } // negative, precision decreased
	++i; if( (i == name_scored_dets->end()) || (cur_score != i->score) ) { break; }
      }
      if( orig_num_pos != num_pos ) // recall increased
      {
	prc_elem_t const prc_elem( num_pos, num_test, cur_score );
	map += prc_elem.get_precision() * (num_pos - orig_num_pos);
	if( num_pos >= next_print ) { 
	  next_print = num_pos + print_skip; 
	  print_prc_line( prc_out, prc_elem, tot_num_class, map / tot_num_class ); 
	}
	prc_elems.push_back( prc_elem );
      }
    }
    map /= tot_num_class;
    if( next_print != (num_pos+print_skip) ) { print_prc_line( prc_out, prc_elems.back(), tot_num_class, map ); }
    (*prc_out ) << strprintf( "---END--- class_name=%s tot_num=%s num_pos=%s num_test=%s num_neg=%s final_map=%s\n", 
			      str(class_name).c_str(),
			      str(tot_num_class).c_str(), 
			      str(num_pos).c_str(), str(num_test).c_str(), 
			      str(num_test - num_pos).c_str(),
			      str(map).c_str() );
    if( !prc_png_fn.empty() ) {
      string const plt_fn = prc_png_fn+class_name+".png";
      prc_plot( plt_fn, tot_num_class, prc_elems, 
		strprintf( "class_name=%s map=%s\n", str(class_name).c_str(), str(map).c_str() ) );
    }

  }

  void img_db_t::score_results( p_vect_p_vect_scored_det_t name_scored_dets_map,
				string const & prc_fn, string const & plot_base_fn ) {
    timer_t t("score_results");
    for( vect_p_vect_scored_det_t::iterator i = name_scored_dets_map->begin(); i != name_scored_dets_map->end(); ++i ) {
      score_results_for_class( *i, prc_fn, plot_base_fn );
    }
  }

  struct is_comma { bool operator()( char const & c ) const { return c == ','; } };

  void read_hamming_csv_file( p_vect_scored_det_t scored_dets, string const & fn, uint32_t const img_ix )
  {
    p_ifstream in = ifs_open( fn );  
    string line;
    while( !ifs_getline( fn, in, line ) )
    {
      vect_string parts;
      split( parts, line, is_comma(), token_compress_on );
      if( (parts.size() == 1) && parts[0].empty() ) { continue; } // skip ws-only lines
      assert( parts.size() == 5 );
      scored_det_t scored_det;
      scored_det.img_ix = img_ix;
      scored_det.p[0].d[0] = uint32_t( lc_str_d( parts[0] ) );
      scored_det.p[0].d[1] = uint32_t( lc_str_d( parts[1] ) );
      scored_det.p[1].d[0] = uint32_t( lc_str_d( parts[2] ) );
      scored_det.p[1].d[1] = uint32_t( lc_str_d( parts[3] ) );
      scored_det.score = lc_str_d( parts[4] );
      scored_det.from_pascal_coord_adjust();
      
      assert_st( scored_det.is_strictly_normalized() );
      scored_dets->push_back( scored_det );
    }
  }


  struct hamming_analysis_t : virtual public nesi, public has_main_t // NESI(help="hamming first-level cascade boxes analysis",bases=["has_main_t"], type_id="ham_ana")
  {
    virtual cinfo_t const * get_cinfo( void ) const; // required declaration for NESI support
    filename_t pascal_classes_fn; //NESI(default="%(boda_test_dir)/pascal_classes.txt",help="file with list of classes to process")
    uint32_t load_imgs; //NESI(default=0,help="if true, load images referenced by the file")
    p_img_db_t img_db; //NESI(default="()", help="image database")
    filename_t pil_fn; //NESI(default="%(pascal_data_dir)/ImageSets/Main/%%s_test.txt",help="format for filenames of image list files. %%s will be replaced with the class name")
    filename_t ham_fn; //NESI(default="%(bench_dir)/hamming/voc-release5-hamming_top1000boxes/2007/%%s_boxes_test__hamming_imgNo%%s.csv",help="format for base filenames of hamming boxes. %%s, %%s will be replaced with the class name, img index")
    filename_t dpm_fn; //NESI(default="%(bench_dir)/hamming/pf_shog/%%s_test.txt",help="format for filenames of pascal-VOC format DPM detection results files. %%s will be replaced with the class name")
    filename_t prc_txt_fn; //NESI(default="%(boda_output_dir)/prc_",help="output: text prc curve base filename")
    filename_t prc_png_fn; //NESI(default="%(boda_output_dir)/mAP_",help="output: png prc curve base filename")

    filename_t score_diff_summary_fn; //NESI(default="%(boda_output_dir)/diff_summ.csv",help="output: text summary of differences between two scored sets.")
    
    virtual void main( nesi_init_arg_t * nia ) { 
      p_vect_string classes = readlines_fn( pascal_classes_fn.exp );
      p_vect_p_vect_scored_det_t hamming_scored_dets( new vect_p_vect_scored_det_t );
      p_vect_p_vect_scored_det_t dpm_scored_dets( new vect_p_vect_scored_det_t );
      
      for( vect_string::const_iterator i = (*classes).begin(); i != (*classes).end(); ++i ) {
	read_pascal_image_list_file( img_db, strprintf( pil_fn.exp.c_str(), (*i).c_str() ), 
				     false, (i != (*classes).begin()) ); 	
      }
      for( vect_string::const_iterator i = (*classes).begin(); i != (*classes).end(); ++i ) {
	hamming_scored_dets->push_back( p_vect_scored_det_t( new vect_scored_det_t( *i ) ) );
	for (uint32_t ix = 0; ix < img_db->img_infos.size(); ++ix) {
	  read_hamming_csv_file( hamming_scored_dets->back(), 
				 strprintf( ham_fn.exp.c_str(), (*i).c_str(), str(ix+1).c_str() ), ix );
	}
	printf( "(*i)=%s (hamming)\n", str((*i)).c_str() );
	dpm_scored_dets->push_back( read_results_file( img_db, strprintf( dpm_fn.exp.c_str(), (*i).c_str() ), *i ) );
	printf( "(*i)=%s (DPM)\n", str((*i)).c_str() );
      }
      img_db->score_results( hamming_scored_dets, prc_txt_fn.exp + "ham_", prc_png_fn.exp + "ham_" );
      img_db->score_results( dpm_scored_dets, prc_txt_fn.exp + "dpm_", prc_png_fn.exp + "dpm_" );
#if 1      
      p_ofstream summ_out = ofs_open( score_diff_summary_fn.exp );  
      (*summ_out) << strprintf( "class_name,num_tot,ham_only,dpm_only,num_ham,num_dpm,num_both,num_either,num_neither,\n");
      for( uint32_t cix = 0; cix != classes->size(); ++cix ) {
	string const & class_name = classes->at(cix);;
	p_vect_scored_det_t ham_sds = hamming_scored_dets->at(cix);
	p_vect_scored_det_t dpm_sds = dpm_scored_dets->at(cix);
	uint32_t ham_only = 0;
	uint32_t dpm_only = 0;
	uint32_t num_ham = 0;
	uint32_t num_dpm = 0;
	uint32_t num_both = 0;
	uint32_t num_neither = 0;
	uint32_t num_tot = 0;
	for (uint32_t ix = 0; ix < img_db->img_infos.size(); ++ix) {
	  p_img_info_t img_info = img_db->img_infos[ix];
	  vect_gt_det_t & gt_dets = img_info->gt_dets[class_name];
	  vect_gt_match_t & ham_gtms = ham_sds->get_gtms( ix, gt_dets.size() ); // must already exist, but not checked
	  vect_gt_match_t & dpm_gtms = dpm_sds->get_gtms( ix, gt_dets.size() ); // must already exist, but not checked
	  bool unmatched = 0;
	  for( uint32_t gtix = 0; gtix < gt_dets.size(); ++gtix ) {
	    ++num_tot;
	    if( ham_gtms.at(gtix).matched ) { ++num_ham; }
	    if( dpm_gtms.at(gtix).matched ) { ++num_dpm; }
	    if( ham_gtms.at(gtix).matched != dpm_gtms.at(gtix).matched ) {
	      if( ham_gtms.at(gtix).matched ) { ++ham_only; }
	      else { ++dpm_only; }
#if 0
	      printf( "unmatched: H=%s D=%s gtix=%s class_name=%s img_info->id=%s\n", 
		      str(ham_gtms.at(gtix).matched).c_str(),
		      str(dpm_gtms.at(gtix).matched).c_str(),
		      str(gtix).c_str(),
		      str(class_name).c_str(), str(img_info->id).c_str() );
#endif
	      unmatched = 1;
	    } else {
	      if( !ham_gtms.at(gtix).matched ) { ++num_neither; }
	      else { ++num_both; }
	    }
	  }
	  bool const show_mismatch_img = 0;
	  if( unmatched && show_mismatch_img ) {
	    p_vect_scored_det_t img_scored_dets( new vect_scored_det_t( class_name ) );
	    for (vect_scored_det_t::const_iterator i = ham_sds->begin(); i != ham_sds->end(); ++i) {
	      if( i->img_ix == ix ) { img_scored_dets->push_back( *i ); }
	    }
	    img_db_show_dets( img_db, img_scored_dets, ix );
	  }
	}
	assert_st( (num_ham+dpm_only) == (num_dpm+ham_only) );
	assert_st( (num_ham+dpm_only) == (num_dpm+ham_only) );
	uint32_t const num_either = num_ham+dpm_only;
	assert_st( num_neither + num_either == num_tot );
	// class num_tot ham_only dpm_only num_ham num_dpm num_both num_either num_neither
	(*summ_out) << strprintf( "%s,%s,%s,%s,%s,%s,%s,%s,%s,\n", str(class_name).c_str(), str(num_tot).c_str(), str(ham_only).c_str(), str(dpm_only).c_str(), str(num_ham).c_str(), str(num_dpm).c_str(), str(num_both).c_str(), str(num_either).c_str(), str(num_neither).c_str() );
      }
#endif
    }
  };

  //p_vect_string readlines_fn( string const & fn ) {


#include"gen/results_io.cc.nesi_gen.cc"

}
